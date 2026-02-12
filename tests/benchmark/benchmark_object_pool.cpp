#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <memory>
#include <atomic>
#include <iomanip>
#include "aegis/common/objectPool.h" // 确保包含你的头文件

using namespace aegis::core;

// 模拟一个负载对象
struct Payload
{
    char data[64]; // 模拟 64 字节对象 (常见的 Cache Line 大小)
    Payload() { data[0] = 'a'; }
    void reset() { data[0] = 'b'; }
};

// 计时器辅助
class Timer
{
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point start_;

public:
    Timer() : start_(Clock::now()) {}
    double elapsed_ms() const
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
    }
};

void benchmark_system_alloc(int thread_count, int iterations)
{
    std::vector<std::thread> threads;
    std::atomic<long> total_dummy{0};

    Timer t;
    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&, iterations]()
                             {
            long dummy = 0;
            for(int j=0; j<iterations; ++j) {
                // 模拟高频申请释放
                volatile Payload* p = new Payload(); 
                dummy += p->data[0];
                delete p;
            }
            total_dummy += dummy; });
    }
    for (auto &th : threads)
        th.join();

    double ms = t.elapsed_ms();
    std::cout << "[System New/Delete] Threads: " << thread_count
              << ", Total Ops: " << (long)thread_count * iterations
              << ", Time: " << ms << " ms"
              << ", OPS: " << std::fixed << std::setprecision(2)
              << ((double)thread_count * iterations / ms * 1000.0) / 1000000.0 << " M/s" << std::endl;
}

void benchmark_object_pool(int thread_count, int iterations)
{
    std::vector<std::thread> threads;
    std::atomic<long> total_dummy{0};

    // 预热 Pool (可选)
    // ObjectPool<Payload>::instance();

    Timer t;
    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&, iterations]()
                             {
            long dummy = 0;
            for(int j=0; j<iterations; ++j) {
                // 使用对象池
                auto ptr = ObjectPool<Payload>::instance().acquire();
                dummy += ptr->data[0];
                // ptr 出作用域自动归还
            }
            total_dummy += dummy; });
    }
    for (auto &th : threads)
        th.join();

    double ms = t.elapsed_ms();
    std::cout << "[Aegis ObjectPool ] Threads: " << thread_count
              << ", Total Ops: " << (long)thread_count * iterations
              << ", Time: " << ms << " ms"
              << ", OPS: " << std::fixed << std::setprecision(2)
              << ((double)thread_count * iterations / ms * 1000.0) / 1000000.0 << " M/s" << std::endl;
}

int main()
{
    std::cout << ">>> Running Benchmarks (Release Mode Recommended) <<<" << std::endl;

    // 场景 1: 单线程基准
    std::cout << "\n--- Single Thread ---\n";
    benchmark_system_alloc(1, 1000000);
    benchmark_object_pool(1, 1000000);

    // 场景 2: 多线程高并发 (4 线程)
    std::cout << "\n--- Multi Thread (4) ---\n";
    benchmark_system_alloc(4, 1000000); // 每个线程 100万次
    benchmark_object_pool(4, 1000000);

    // 场景 3: 极限并发 (8 线程) - 观察锁竞争带来的影响
    std::cout << "\n--- Multi Thread (8) ---\n";
    benchmark_system_alloc(8, 1000000);
    benchmark_object_pool(8, 1000000);

    return 0;
}