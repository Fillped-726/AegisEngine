#include "aegis/common/aegisLog.h" // 确保路径正确
#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <fstream>
#include <format>
#include <iomanip>

// 简单的计时器
class Stopwatch
{
    using Clock = std::chrono::high_resolution_clock;
    std::chrono::time_point<Clock> start_time;

public:
    Stopwatch() : start_time(Clock::now()) {}
    double elapsed_ms()
    {
        auto end_time = Clock::now();
        return std::chrono::duration<double, std::milli>(end_time - start_time).count();
    }
};

const int LOG_COUNT = 500000; // 保持 50万条

// 修改返回值为 double，返回耗时(ms)
double bench_legacy_iostream()
{
    std::cerr << "[Bench] Starting Legacy iostream (Sync)..." << std::endl;
    Stopwatch sw;
    for (int i = 0; i < LOG_COUNT; ++i)
    {
        std::cout << "[Legacy] Index: " << i << " Payload: " << "SomeData" << " Float: " << 3.14f << "\n";
    }
    double cost = sw.elapsed_ms();
    // std::cerr << "[Bench] Legacy iostream finished: " << cost << " ms" << std::endl;
    return cost;
}

double bench_modern_cout()
{
    std::cerr << "[Bench] Starting std::format + cout (Sync)..." << std::endl;
    Stopwatch sw;
    for (int i = 0; i < LOG_COUNT; ++i)
    {
        std::cout << std::format("[Modern] Index: {} Payload: {} Float: {}\n", i, "SomeData", 3.14f);
    }
    double cost = sw.elapsed_ms();
    // std::cerr << "[Bench] Modern cout finished: " << cost << " ms" << std::endl;
    return cost;
}

double bench_aegis_async()
{
    std::cerr << "[Bench] Starting AegisLog (Async)..." << std::endl;
    aegis::Log::instance().init_config("logs/bench.log", "Bench");

    Stopwatch sw;
    for (int i = 0; i < LOG_COUNT; ++i)
    {
        aegis::Log::instance().info("Index: {} Payload: {} Float: {}", i, "SomeData", 3.14f);
    }

    double cost = sw.elapsed_ms();
    // std::cerr << "[Bench] AegisLog (Worker Thread Release): " << cost << " ms" << std::endl;
    return cost;
}

struct BenchResult
{
    std::string name;
    double cost_ms;
    double qps;
    double speedup;
};

void print_summary(const std::vector<BenchResult> &results)
{
    std::cerr << "\n\n=================================================================================\n";
    std::cerr << "                               BENCHMARK SUMMARY                                 \n";
    std::cerr << "=================================================================================\n";
    std::cerr << std::left << std::setw(25) << "Method"
              << std::right << std::setw(15) << "Time (ms)"
              << std::right << std::setw(15) << "QPS (msg/s)"
              << std::right << std::setw(15) << "Speedup (x)"
              << "\n";
    std::cerr << "---------------------------------------------------------------------------------\n";

    for (const auto &res : results)
    {
        std::cerr << std::left << std::setw(25) << res.name
                  << std::right << std::setw(15) << std::fixed << std::setprecision(2) << res.cost_ms
                  << std::right << std::setw(15) << (int)res.qps
                  << std::right << std::setw(15) << std::fixed << std::setprecision(2) << res.speedup
                  << "\n";
    }
    std::cerr << "=================================================================================\n";
    std::cerr << "* Speedup is relative to Legacy iostream (Baseline)\n\n";
}

int main()
{
    std::ios::sync_with_stdio(false);

    std::cerr << "=== Benchmark Running (Count: " << LOG_COUNT << ") ===\n";
    std::cerr << "!!! Output is redirected to stdout, stats to stderr !!!\n\n";

    // 1. Run Tests
    double t_aegis = bench_aegis_async();
    double t_modern = bench_modern_cout();
    double t_legacy = bench_legacy_iostream();

    // 2. Calculate Stats
    std::vector<BenchResult> results;

    // Baseline
    results.push_back({"Legacy iostream", t_legacy, (LOG_COUNT / t_legacy) * 1000.0, 1.0});

    // Modern
    results.push_back({"Modern std::format", t_modern, (LOG_COUNT / t_modern) * 1000.0, t_legacy / t_modern});

    // Aegis
    results.push_back({"AegisLog (Async)", t_aegis, (LOG_COUNT / t_aegis) * 1000.0, t_legacy / t_aegis});

    // 3. Print Report
    print_summary(results);

    return 0;
}