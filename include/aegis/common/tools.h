/**
 * @file tools.h
 * @brief Miscellaneous utility functions (fd helpers, string ops, time).
 */
#pragma once
#include <pthread.h> // [DEPENDENCY: POSIX threads]
#include <sys/resource.h> // [DEPENDENCY: POSIX resource limits]
#include <iostream>
#include <cstdio>

// [STATE_MUTATION: Pin calling thread OS scheduling affinity to target CPU core]
void bind_to_core(int core_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// [STATE_MUTATION: Elevate process RLIMIT_NOFILE soft limit to system hard limit]
void tune_fd_limit()
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
    {
        perror("getrlimit");
        return;
    }

    rl.rlim_cur = rl.rlim_max;

    if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
    {
        perror("setrlimit"); 
    }
    else
    {
        std::cout << "Successfully raised FD limit to: " << rl.rlim_cur << std::endl;
    }
}