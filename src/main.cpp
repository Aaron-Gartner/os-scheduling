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
            }
        }
        // - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        for (int i = 0; i < processes.size(); i++) {
            {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            //Moves from I/O to back into the queue
            if (processes[i]->getState()  == Process::State::IO) {
                processes[i]->setState(Process::State::Ready,current);
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
            if (processes[i]->getRemainingTime() >= shared_data->time_slice && processes[i]->Running) {
                    processes[i]->interrupt();
                    index++;
                    processes[i]->setState(Process::State::IO,current);
                    processes[i]->updateProcess(current);
                    shared_data->ready_queue.push_back(processes[i]);
                }     
            }
            /*
            if (processes[i]->getPriority() >= shared_data->ready_queue.front()->getPriority() && processes[i]->Running) {
                processes[i]->interrupt();
                index++;
                processes[i]->setState(Process::State::IO,current);
                processes[i]->updateProcess(current);
                shared_data->ready_queue.push_back(processes[i]);
                //may or may not need this
                processes[i] = shared_data->ready_queue.front();
                shared_data->ready_queue.pop_front();
            }*/
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
        }
        //   - Determine if all processes are in the terminated state
        bool allComplete = false;
        int counter = 0;
        for (int i = 0; i < processes.size(); i++) {
            //Takes the process at the front of the ready queue
            if (processes[i]->getState() == Process::State::Terminated) {
                allComplete = true;
                counter++;
            } else {
                allComplete = false;
            }   
        }
        //check if all complete is true and it occured for everything in our ready queue
        if (allComplete == true && counter == shared_data->ready_queue.size()) {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            shared_data->all_terminated = allComplete;
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

    // print final statistics
    std::list<Process*>::iterator it;
    //  - CPU utilization = 1 - (average percentage of time processes are waiting for I/O) * (number of processes running in memory)
    //printf("CPU Utilization: %f\n");
    //  - Throughput = cpu time of each process added together, then divided by number of processes
    //     - Average for first 50% of processes finished
    //printf("Average throughput for first 50% of processes finished: %f\n");
    //     - Average for second 50% of processes finished
    //printf("Average throughput for second 50% of processes finished: %f\n");
    //     - Overall average
    double overallThroughput = 0.0;
    for (int i = 0; i < processes.size(); i++) {
        overallThroughput = overallThroughput + processes[i]->getCpuTime();
    }
    overallThroughput = overallThroughput / (double)config->num_processes;
    //printf("Overall average throughput: %f\n");
    //  - Average turnaround time
    //printf("Average turnaround time: %f\n");
    //  - Average waiting time
    double waitAverage = 0.0;
    for (int i = 0; i < processes.size(); i++) {
        waitAverage = waitAverage + processes[i]->getWaitTime();
    }
    waitAverage = waitAverage / (double)config->num_processes;
    printf("Average wait time: %f",waitAverage);


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
        
        int index = 0;
        Process *front = NULL;
        {
        //check if ready queue is empty or not
        std::lock_guard<std::mutex> lock(shared_data->mutex);
        if (!(shared_data->ready_queue.empty())){
            front = shared_data->ready_queue.front();
            shared_data->ready_queue.remove(front);
            //printf("Here\n");
            }
        }
        if (front != NULL) {
            front->setCpuCore(core_id); 
            //printf("Here\n");
            front->setState(Process::State::Running,currentTime());
            //must compare cpu burst
            //switch out with while loop(whle not interrupted and time running < cpu burst) add 10 milsec sleep
            printf("Num_Bursts:%d\n", front->get_bursts());
            while(!(front->isInterrupted()) && (index+1 != front->get_bursts())) {
                printf("Num_Bursts:%d\n", front->get_bursts());
                //start timer
                uint64_t startTimer = currentTime();
                //printf("Here\n");
                if ((front->getRemainingTime() <= 0.0) && front->getState() == Process::State::Running) {
                    {
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    index++;
                    front->setState(Process::State::Terminated,currentTime());
                    front->setCpuCore(-1);
                    }
                }
                //only occurs if interrupted
                if  (front->isInterrupted()) {
                    front->setState(Process::State::IO,currentTime());
                    front->updateBurstTime(index, currentTime());
                    shared_data->ready_queue.push_back(front);
                }
                 // sleep 5 ms
                usleep(5000);
            }
            //end timer  
            uint64_t endTimer = currentTime();
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
