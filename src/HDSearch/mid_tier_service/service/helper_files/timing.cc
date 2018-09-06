#include <cstddef>
#include <fstream>
#include <iostream>
#include <numeric>
#include "timing.h"
#include <vector>

uint64_t GetTimeInMicro()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec*(uint64_t)1000000+tv.tv_usec);
}

uint64_t GetTimeInSec()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

double Rdtsc()
{
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    // A.S TODO: Get the frequency using lscpu
    uint64_t frequency = 1200632000;
    double seconds = (double)(((uint64_t)hi << 32) | lo)/(double)frequency;
    return seconds;
}

uint64_t RdtscCycles()
{
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return (((uint64_t)hi << 32) | lo);
}

void GetCpuTimes(uint64_t* user_time,
        uint64_t* system_time,
        uint64_t* io_time,
        uint64_t* idle_time)
{
    std::ifstream proc_stat("/proc/stat");
    proc_stat.ignore(5, ' '); // Skip the 'cpu' prefix.
    std::vector<size_t> cpu_times;
    for (size_t time; proc_stat >> time; cpu_times.push_back(time));
    if (cpu_times.size() < 4)
    {
        std::cout << "Size of CPU times cannot be less than 4\n";
        exit(0);
    }
    (*user_time) = cpu_times[0] + cpu_times[1];
    (*system_time) = cpu_times[2];
    (*io_time) = cpu_times[4] + cpu_times[5] + cpu_times[6];
    (*idle_time) = cpu_times[3];

}

