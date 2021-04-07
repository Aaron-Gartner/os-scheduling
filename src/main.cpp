#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <iterator>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"


// Shared data for all cores
typedef struct SchedulerData {
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    // Free configuration data from memory
    deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    int num_lines = 0;
    while (!(shared_data->all_terminated))
    {   // Do the following:
        // - Get current time
        uint64_t current = currentTime();
         //Iterator to access our processes
        std::list<Process*>::iterator it;
        // - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        for(int i = 0; i < processes.size(); i++){
            if(processes[i]->getState() == Process::State::NotStarted && (current - start) >= processes[i]->getStartTime()){
                processes[i]->setState(Process::State::Ready, current);
                shared_data->ready_queue.push_back(processes[i]);
            }
        }
        // - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        for (int i = 0; i < processes.size(); i++) {
            {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            //Moves from I/O to back into the queue
            if ((current - processes[i]->getBurstStartTime()) >= processes[i]->getBurstTime(processes[i]->getCurrentBurstIndex()) && processes[i]->getState()  == Process::State::IO) {
                processes[i]->setState(Process::State::Ready,current);
                shared_data->ready_queue.push_back(processes[i]);
            }
            }
        }
        // - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
        //Check if state is running looping over all processes
        int index = 0;
        for (int i = 0; i < processes.size(); i++) {
            {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            //time the process been on cpu longer than time slice
            if ((current - processes[i]->getBurstStartTime()) >= shared_data->time_slice && processes[i]->getState() == Process::State::Running) {
                    processes[i]->interrupt();
                    processes[i]->updateProcess(current);
                }     
            }
        }
        // - *Sort the ready queue (if needed - based on scheduling algorithm)
        //Sort based on remaining time
        if (shared_data->algorithm == SJF) {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            shared_data->ready_queue.sort(SjfComparator());
        } 
        //Sort based on priority
        if (shared_data->algorithm == PP) {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            shared_data->ready_queue.sort(PpComparator());
            for (int i = 0; i < processes.size(); i++) {
                {
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                Process* front = shared_data->ready_queue.front();
                if ((processes[i]->getPriority() > front->getPriority() && processes[i]->getState() == Process::State::Running)) {
                        processes[i]->interrupt();
                        processes[i]->updateProcess(current);
                        shared_data->ready_queue.push_back(processes[i]);
                    }     
                }
            }
        }
        //   - Determine if all processes are in the terminated state
        int counter = 0;
        for (int i = 0; i < processes.size(); i++) {
            {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            if (processes[i]->getState() == Process::State::Terminated) {
                counter++;
            }
            }   
        }
        //check if all complete is true and it occured for everything in our ready queue
        if (counter == processes.size()) {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            shared_data->all_terminated = true;
        }
        //updates Processes
        for (int i = 0; i < processes.size(); i++){
            {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            processes[i]->updateProcess(current);
            }
        }
        // Clear output from previous iteration
        clearOutput(num_lines);

        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);

        // sleep 50 ms
        usleep(50000);
    }


    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    uint64_t end = currentTime();

    // print final statistics
    std::list<Process*>::iterator it;
    //  - CPU utilization = 1 - (average percentage of time processes are waiting for I/O) * (number of processes running in memory)
    double waitTime = 0.0;
    for (int i = 0; i < processes.size(); i++) {
        waitTime = waitTime + processes[i]->getWaitTime();
    }
    waitTime = waitTime / (double)processes.size();
    double averageWaitPercentage = waitTime/(end - start);
    double cpuUtil = (1 - averageWaitPercentage * processes.size()) * 100.0;
    printf("CPU Utilization: %f\n", cpuUtil);

    //  - Throughput = cpu time of each process added together, then divided by number of processes
    double overallThroughput = 0.0;
    for (int i = 0; i < processes.size(); i++) {
        overallThroughput = overallThroughput + processes[i]->getCpuTime();
    }
    overallThroughput = overallThroughput / (double)processes.size();
    //     - Average for first half of processes finished
    double firstHalfThroughput = 0.0;
    int numProcesses = 0;
    for (int i = 0; i < processes.size(); i++) {
        if(processes[i]->getCpuTime() <= overallThroughput){
            firstHalfThroughput = firstHalfThroughput + processes[i]->getCpuTime();
            numProcesses++;
        }
    }
    firstHalfThroughput = firstHalfThroughput / numProcesses;
    printf("Average throughput for first half of processes finished: %f\n", firstHalfThroughput);
    //     - Average for second half of processes finished
    double secondHalfThroughput = 0.0;
    numProcesses = 0;
    for (int i = 0; i < processes.size(); i++) {
        if(processes[i]->getCpuTime() >= overallThroughput){
            secondHalfThroughput = secondHalfThroughput + processes[i]->getCpuTime();
            numProcesses++;
        }
    }
    secondHalfThroughput = secondHalfThroughput / numProcesses;
    printf("Average throughput for second half of processes finished: %f\n", secondHalfThroughput);
    //     - Overall average
    printf("Overall average throughput: %f\n", overallThroughput);

    //  - Average turnaround time
    double averageTurnaround = 0.0;
    for (int i = 0; i < processes.size(); i++) {
        averageTurnaround = averageTurnaround + processes[i]->getTurnaroundTime();
    }
    averageTurnaround = averageTurnaround / (double)processes.size();
    printf("Average turnaround time: %f\n", averageTurnaround);

    //  - Average waiting time
    printf("Average wait time: %f\n",waitTime);


    // Clean up before quitting program
    processes.clear();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{   // Work to be done by each core idependent of the other cores
    // Repeat until all processes in terminated state:
    //   - *Get process at front of ready queue
    //   - Simulate the processes running until one of the following:
    //     - CPU burst time has elapsed
    //     - Interrupted (RR time slice has elapsed or process preempted by higher priority process)
    //  - Place the process back in the appropriate queue
    //     - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO
    //     - Terminated if CPU burst finished and no more bursts remain -- no actual queue, simply set state to Terminated
    //     - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)
    //  - Wait context switching time
    //  - * = accesses shared data (ready queue), so be sure to use proper synchronization
    
    //take the thing at the front of the ready queue
    while (!(shared_data->all_terminated)) {    
        Process *front = NULL;
        {
        //check if ready queue is empty or not
        std::lock_guard<std::mutex> lock(shared_data->mutex);
        if (!(shared_data->ready_queue.empty())){
            front = shared_data->ready_queue.front();
            shared_data->ready_queue.pop_front();
            }
        }
        if (front != NULL) {
            {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            front->setCpuCore(core_id); 
            }
            //printf("Here\n");
            uint64_t now = currentTime();
            front->setState(Process::State::Running,now);
            front->setBurstStartTime(now);   
            while(!(front->isInterrupted()) && ((currentTime() - now) < front->getBurstTime(front->getCurrentBurstIndex()))) {
                //start time
                uint64_t startTimer = currentTime();
                front->setCpuUtilizationStartTime(startTimer);
                 // sleep 10 ms
                usleep(10000);
            }
            if (front->isInterrupted() && ((currentTime() - now) < front->getBurstTime(front->getCurrentBurstIndex()))) {
                {
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                front->setState(Process::State::Ready,currentTime());
                front->setCpuCore(-1);
                front->updateBurstTime(front->getCurrentBurstIndex(), front->getBurstTime(front->getCurrentBurstIndex()) - (currentTime() - now));
                front->interruptHandled();
                shared_data->ready_queue.push_back(front);
                }
            }
            else if (front->isFinalBurst(front->getCurrentBurstIndex())) {    
                front->setState(Process::State::Terminated,currentTime());
                //set time remaining
                front->setCpuCore(-1);
            } else {
                front->setState(Process::State::IO, currentTime());
                //front->updateBurstTime(front->getCurrentBurstIndex()+1, currentTime());
                front->setCpuCore(-1);
            }
                //only occurs if interrupted
            //end timer  
            front->setCpuUtilizationTime(currentTime());

            // = cpuUtilTime + (endTimer - startTimer);
            //sleep context switch
            usleep(shared_data->context_switch);
            //figure out cpu util
            
        }

    }   
}

int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n", 
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time, 
                   wait_time, cpu_time, remain_time);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}