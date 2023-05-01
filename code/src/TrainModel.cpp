#include "Instance.hpp"
#include "MAPFLoader.hpp"
#include "CBSSolver.hpp"
#include "TesterUtils.hpp"
#include "DataLoader.hpp"

#include <argparse/argparse.hpp>
using namespace std;

void writeMetricsToFile(trainMetrics metrics, string filePath){
    ofstream file(filePath, std::ios_base::app);
    if (file.is_open())
    {   
        file << metrics.sumOfCosts << " " << metrics.elapsedTime << " " << metrics.counter << " " << metrics.numConstraint << "\n";
        file.close();
    } else cout << "Problem with opening file";
}

trainMetrics runOneInstance(MAPFInstance& mapfProblem, DataLoader& trainLoader, int& counter, TestTimer& ttimer, torch::optim::Adam& optimizer, ConfNet* modelPtr, torch::Device device){

    //Input maps
    torch::Tensor collisionMap = torch::zeros({mapfProblem.rows, mapfProblem.cols}, device);
    torch::Tensor instanceMap = torch::zeros({mapfProblem.rows, mapfProblem.cols}, device);
    torch::Tensor startMap = torch::zeros({mapfProblem.rows, mapfProblem.cols}, device);
    torch::Tensor goalMap = torch::zeros({mapfProblem.rows, mapfProblem.cols}, device);
    
    for(int i=0; i<mapfProblem.rows; i++){
        for(int j=0; j<mapfProblem.cols; j++){
            instanceMap[i][j] = int(mapfProblem.map[i][j]);
        }  
    }
    
    for(int i=0; i<mapfProblem.numAgents; i++){
        Point2 startLoc = mapfProblem.startLocs[i];
        Point2 goalLoc = mapfProblem.goalLocs[i];
        startMap[mapfProblem.startLocs[i].x][mapfProblem.startLocs[i].y] = 1;
        goalMap[mapfProblem.goalLocs[i].x][mapfProblem.goalLocs[i].y] = 1;
    }
    
    torch::Tensor inputMaps = torch::stack({collisionMap, instanceMap, startMap, goalMap}, 0);
    inputMaps = torch::unsqueeze(inputMaps, 0);
    inputMaps.to(device);
    
    // Create CBS solver 
    CBSSolver cbsSolver;
    counter =  0;
    ttimer.start();

    //track metrics
    trainMetrics metrics;
    metrics.counter =  0;
    metrics.runningLoss = 0;
    metrics.numLoss = 0;

    bool timeout = false;
    optimizer.zero_grad();
    
    auto optNode = cbsSolver.trainSolve(mapfProblem, timeout, trainLoader.constraintTensor, modelPtr, optimizer, inputMaps, metrics, device);
    optimizer.step();
    std::vector<std::vector<Point2>> paths = optNode->paths;

    metrics.elapsedTime = ttimer.elapsed();
    metrics.sumOfCosts=0;
    for(const auto& path : paths)
        metrics.sumOfCosts += path.size()-1;
    std::vector<Constraint> constraintList = optNode->constraintList;
    metrics.numConstraint = constraintList.size();
    metrics.avgLoss = metrics.runningLoss/static_cast<float>(metrics.numLoss);
    
    return metrics;
}



int main(int argc, char *argv[])
{
    argparse::ArgumentParser program("Model Training");

    program.add_argument("--num_train").help("number of train instances to use")
        .default_value(5000).scan<'i', int>();
    
    program.add_argument("--train_path").help("path to load train instances")
        .default_value(string("../../data/instances/train_instances/"));
    
    program.add_argument("--train_label_path").help("path to load train data")
        .default_value(string{"../../data/labels/train_labels/"});

    program.add_argument("--num_test").help("number of test instances to use")
        .default_value(500).scan<'i', int>();

    program.add_argument("--test_path").help("path to load test instances")
        .default_value(string{"../../data/instances/test_instances/"});

    program.add_argument("--test_label_path").help("path to load test data")
        .default_value(string{"../../data/labels/test_labels/"});

    program.add_argument("--output_path").help("path to outputs")
        .default_value(string{"../../data/outputs/train_outputs.txt"});
    
    program.add_argument("--model_path").help("path to models")
        .default_value(string{"../../data/models/"});
    
    program.add_argument("--lr").help("learning rate for model training")
        .default_value(0.002f).scan<'f', float>();
    
    program.add_argument("--eval_freq").help("frequecy to save model with")
        .default_value(1000).scan<'i', int>();

    try
    {
        program.parse_args(argc, argv);
    }
    catch (const runtime_error &err)
    {
        cerr << err.what() << endl;
        cerr << program;
        return 1;
    }
    // Device
    auto cuda_available = torch::cuda::is_available();
    torch::Device device(cuda_available ? torch::kCUDA : torch::kCPU);
    std::cout << (cuda_available ? "CUDA available. Training on GPU." : "Training on CPU.") << '\n';

    const double learning_rate = program.get<float>("lr");
    const double weight_decay = 1e-3;
    const size_t num_epochs = 3;

    string instancePath = program.get<string>("train_path");
    string labelPath = program.get<string>("train_label_path");
    string t_instancePath = program.get<string>("test_path");
    string t_labelPath = program.get<string>("test_label_path");
    string outPath = program.get<string>("output_path");
    string modelPath = program.get<string>("model_path");

    int counter;
    bool timeout;
    int id;
    bool train = true;

    TestTimer ttimer;
    DataLoader trainLoader;
    DataLoader testLoader;
    MAPFLoader loader;
    float bestLoss = 100;
    std::vector<float> val_losses;

    // Model
    string filePath = instancePath + to_string(0) + ".txt";
    MAPFInstance mapfProblem = loader.loadInstanceFromFile(filePath);

    ConfNet model(mapfProblem.cols, mapfProblem.rows, 64, 1), *modelPtr;
    modelPtr = &model;
    model->to(device);
    model->train();

    // Optimizer
    torch::optim::Adam optimizer(
        model->parameters(), torch::optim::AdamOptions(learning_rate).weight_decay(weight_decay));

    // Set floating point output precision
    std::cout << std::fixed << std::setprecision(4);

    for (int epoch = 0; epoch != num_epochs; ++epoch) {
        cout<<"---------------------EPOCH "<<epoch<<"---------------------\n";
        for(int i=0; i<program.get<int>("num_train"); i++)
        {   
            printf("Epoch: %d, Processing sample: %d\n", epoch, i);

            filePath = instancePath + to_string(i) + ".txt";
            mapfProblem = loader.loadInstanceFromFile(filePath);
            
            filePath = labelPath + to_string(i) + ".txt";
            trainLoader.loadDataFromFile(filePath);

            trainMetrics metrics = runOneInstance(mapfProblem, trainLoader, counter, ttimer, optimizer, modelPtr, device);
            cout<<"TRAIN RESULTS | elapsedTime: " << metrics.elapsedTime << ", counter: " << metrics.counter << ", numConstraint: " << metrics.numConstraint << ", loss: " <<  metrics.avgLoss << "\n";
            cout<<"ORIGI RESULTS | elapsedTime: " << trainLoader.metrics.elapsedTime << ", counter: " << trainLoader.metrics.counter << ", numConstraint: " << trainLoader.metrics.numConstraint << "\n";
            
            filePath = outPath;
            writeMetricsToFile(metrics, filePath);
            
            if(i%program.get<int>("eval_freq")==0){
                string currPath = modelPath + "epoch_" + to_string(epoch) + "_sample_" + to_string(i) + ".pt";
                torch::save(model, currPath);

                // cout<<"---------------------VALIDATION START---------------------\n";

                // model->eval();

                // for (int j=program.get<int>("num_train"); j<program.get<int>("num_train")+program.get<int>("num_test"); j++){
                //     filePath = t_instancePath + to_string(j) + ".txt";
                //     mapfProblem = loader.loadInstanceFromFile(filePath);
                    
                //     filePath = t_labelPath + to_string(j) + ".txt";
                //     testLoader.loadDataFromFile(filePath);

                //     trainMetrics metrics = runOneInstance(mapfProblem, testLoader, counter, ttimer, optimizer, modelPtr, device);
                //     cout<<" loss: " << metrics.avgLoss<< "\n";
                //     val_losses.push_back(metrics.avgLoss);
                // }

                // float avg_loss = std::accumulate(val_losses.begin(), val_losses.end(), 0.0) / val_losses.size();

                // cout<<" LOSS: " << avg_loss<< "\n";

                // if(avg_loss<bestLoss){
                //     torch::save(model, modelPath+"best.pt");
                // }

                // torch::save(model, modelPath+"latest.pt");
                // std::vector<float> val_losses;
                // val_losses.clear();
                // cout<<"---------------------VALIDATION END---------------------\n";
                // model->train();
            }
        }
    }
    return 0;
};