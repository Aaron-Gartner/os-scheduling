#include "process.h"

// Process class methods
Process::Process(ProcessDetails details, uint64_t current_time)
{
    int i;
    pid = details.pid;
    start_time = details.start_time;
    num_bursts = details.num_bursts;
    current_burst = 0;
    burst_times = new uint32_t[num_bursts];
    for (i = 0; i < num_bursts; i++)
    {
        burst_times[i] = details.burst_times[i];
    }
    priority = details.priority;
    state = (start_time == 0) ? State::Ready : State::NotStarted;
    if (state == State::Ready)
    {   
        state_startT = current_time;
        launch_time = current_time;
    }
    is_interrupted = false;
    core = -1;
    turn_time = 0;
    wait_time = 0;
    cpu_time = 0;
    cpu_completed_bursts = 0;
    remain_time = 0;
    total_waiting_time = 0;
    cpu_utilization_time = 0;
    cpu_utilization_start = 0;
    for (i = 0; i < num_bursts; i+=2)
    {
        remain_time += burst_times[i];
    }
    total_run_time = remain_time;

}
 

Process::~Process()
{
    delete[] burst_times;
}


uint16_t Process::getPid() const
{
    return pid;
}

uint32_t Process::getStartTime() const
{
    return start_time;
}

uint8_t Process::getPriority() const
{
    return priority;
}

uint64_t Process::getBurstStartTime() const
{
    return burst_start_time;
}

Process::State Process::getState() const
{
    return state;
}

bool Process::isInterrupted() const
{   //if time slice expired/if higher prior == true
    return is_interrupted;
}

int8_t Process::getCpuCore() const
{
    return core;
}

double Process::getTurnaroundTime() const
{
    return (double)turn_time / 1000.0;
}

double Process::getWaitTime() const
{
    return (double)wait_time / 1000.0;
}

double Process::getCpuTime() const
{
    return (double)cpu_time / 1000.0;
}

double Process::getRemainingTime() const
{
    return (double)remain_time / 1000.0;
}

uint32_t Process::getCurrentBurstIndex(){

    return current_burst;

}

void Process::setBurstStartTime(uint64_t current_time)
{
    burst_start_time = current_time;
}

uint64_t Process::getCpuUtilizationStartTime() 
{
    return cpu_utilization_start;
}

void Process::setCpuUtilizationStartTime(uint64_t currentTime) {
    cpu_utilization_start =  currentTime;
}

double Process::getCpuUtilizationTime() 
{
    return (double)cpu_utilization_time/1000.0;
}

void Process::setCpuUtilizationTime(uint64_t endTime) {
    cpu_utilization_time = cpu_utilization_time +(endTime - cpu_utilization_start);
}

void Process::setState(State new_state, uint64_t current_time)
{
    if (state == State::NotStarted && new_state == State::Ready)
    {
        launch_time = current_time;
    }
    if (state == State::Running && new_state != State::Running)
    {
        cpu_completed_bursts = cpu_time;
    }
    if (state == State::Ready && new_state != State::Ready)
    {
        total_waiting_time = wait_time;
    }
    if (state == State::IO && new_state == State::Ready)
    {
        current_burst++;
    }
    if (state == State::Running && new_state == State::IO)
    {
        current_burst++;
    }
    if (state == State::Running && new_state == State::Terminated)
    {
        setRemainingTime();
    }

    state_startT = current_time;

    state = new_state;
}

void Process::setCpuCore(int8_t core_num)
{
    core = core_num;
}

void Process::interrupt()
{
    is_interrupted = true;
}

void Process::interruptHandled()
{
    is_interrupted = false;
}

void Process::updateProcess(uint64_t current_time)
{   
    // use `current_time` to update turnaround time, wait time, burst times, 
    // cpu time, and remaining time
    // current time update
    uint32_t update_time_elapsed = current_time - state_startT;
    // updates time run on cpu
    if (state == State::Running) {
        cpu_time = cpu_completed_bursts + update_time_elapsed;
        
    }
    //updates the remaining time base on time run on cpu
    remain_time = (total_run_time - cpu_time);
    //updates wait time
    if(state == State::Ready) { 
        wait_time = total_waiting_time + update_time_elapsed;
    }
    //updates turn time 
    if(state != State::Terminated) {
        turn_time = current_time - launch_time;
    }
}

//only used if process is interrupted
void Process::updateBurstTime(int burst_idx, uint32_t new_time)
{
    burst_times[burst_idx] = new_time;
    current_burst = burst_idx;
}


uint32_t Process::getBurstTime(int index)
{ 
    return burst_times[index];
}

uint16_t Process::get_bursts() {
    return num_bursts;
}

bool Process::isFinalBurst(int index) {
    if (index+1 == num_bursts) {
        return true;
    } else {
        return false;
    }
}

void Process::setRemainingTime(){
    remain_time = 0;
}


// Comparator methods: used in std::list sort() method
// No comparator needed for FCFS or RR (ready queue never sorted)

// SJF - comparator for sorting read queue based on shortest remaining CPU time
bool SjfComparator::operator ()(const Process *p1, const Process *p2)
{
    // your code here!
    if (p1->getRemainingTime() > p2->getRemainingTime()){
        return true;
    }
    return false; 
}

// PP - comparator for sorting read queue based on priority
bool PpComparator::operator ()(const Process *p1, const Process *p2)
{
    // your code here!
    if (p1->getPriority() > p2->getPriority()){
        return true;
    }
    return false; 
}