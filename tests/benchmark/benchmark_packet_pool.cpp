#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <random>
#include <iomanip>

// --- 引入你的头文件 ---
// 假设你的头文件路径配置好了，或者你把它们和这个cpp放一起
#include "aegis/net/packet.h"
#include "aegis/common/objectPool.h"

using namespace aegis::net;
using namespace aegis::core;

// --- 补全 Packet 的最小实现 (Mock) ---
// 如果你已经链接了 libaegis-engine.a，这些可以删掉
namespace aegis::net
{
    Packet::Packet(const Packet &other) { /* 略 */ }
    Packet &Packet::operator=(const Packet &other) { return *this; }
    Packet::Packet(Packet &&other) noexcept { /* 略 */ }
    Packet &Packet::operator=(Packet &&other) noexcept { return *this; }

    // 简单的析构
    Packet::~Packet() { free_heap(); }

    void Packet::alloc(size_t req_size)
    {
        if (req_size <= kSmallBufferSize)
        {
            data_ = stack_buf_;
        }
        else
        {
            if (capacity_ < req_size)
            {
                free_heap();
                heap_buf_ = new char[req_size];
                capacity_ = req_size;
            }
            data_ = heap_buf_;
        }
        size_ = req_size;
    }

    void Packet::free_heap()
    {
        if (heap_buf_)
        {
            delete[] heap_buf_;
            heap_buf_ = nullptr;
        }
    }

    // [重要] 确保这是 Public 的，以便 Pool 调用
    void Packet::reset()
    {
        size_ = 0;
        data_ = stack_buf_;
        // 保留大内存不释放，或者按需释放
        // 这里模拟最激进的策略：保留内存，不 delete
    }
}

// --- 简单的防优化黑洞 ---
volatile char g_dummy_sink;

// --- 计时器封装 ---
template <typename Func>
double measure_ms(const std::string &name, int iterations, Func &&func)
{
    auto start = std::chrono::high_resolution_clock::now();

    func();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;

    double avg_ns = (elapsed.count() * 1000000.0) / iterations;

    std::cout << std::left << std::setw(30) << name
              << ": " << std::setw(10) << elapsed.count() << " ms"
              << " | Avg: " << avg_ns << " ns/op" << std::endl;

    return elapsed.count();
}

int main()
{
    const int N = 1000000; // 100万次操作
    std::cout << "=== Benchmark Start (Iterations: " << N << ") ===" << std::endl;
    std::cout << "Packet Size (SBO Limit): " << kSmallBufferSize << " bytes" << std::endl;

    // 预热 ObjectPool
    {
        auto warmup = ObjectPool<Packet>::instance().acquire();
        warmup->alloc(100);
    }

    // --- 场景 1: 小包 (SBO 命中) ---
    // 模拟心跳包或移动包，完全在栈上/对象内部
    std::cout << "\n--- [Scenario 1: Small Packet (128 bytes, SBO Hit)] ---" << std::endl;

    measure_ms("Raw New/Delete", N, [&]()
               {
        for(int i=0; i<N; ++i) {
            Packet* pkt = new Packet();
            pkt->alloc(128); 
            // 模拟写入，强制发生 Cache 访问
            pkt->mutable_data()[0] = (char)i; 
            g_dummy_sink = pkt->data()[0];
            delete pkt;
        } });

    measure_ms("ObjectPool", N, [&]()
               {
        auto& pool = ObjectPool<Packet>::instance();
        for(int i=0; i<N; ++i) {
            auto pkt = pool.acquire(); // 获取
            pkt->alloc(128);           // 只是指针偏移，无 malloc
            pkt->mutable_data()[0] = (char)i;
            g_dummy_sink = pkt->data()[0];
            // 离开作用域自动 release
        } });

    // --- 场景 2: 大包 (SBO Miss) ---
    // 模拟玩家数据同步，需要额外的堆分配
    std::cout << "\n--- [Scenario 2: Large Packet (4096 bytes, Heap Alloc)] ---" << std::endl;

    measure_ms("Raw New/Delete", N, [&]()
               {
        for(int i=0; i<N; ++i) {
            Packet* pkt = new Packet();
            pkt->alloc(4096); // 触发内部 malloc
            pkt->mutable_data()[0] = (char)i;
            g_dummy_sink = pkt->data()[0];
            delete pkt;
        } });

    measure_ms("ObjectPool", N, [&]()
               {
        auto& pool = ObjectPool<Packet>::instance();
        for(int i=0; i<N; ++i) {
            auto pkt = pool.acquire();
            // 注意：虽然 Packet 对象本身复用了，但 alloc(4096) 
            // 依然可能会触发内部的 new char[]，除非 Packet::reset 保留了 buffer
            pkt->alloc(4096); 
            pkt->mutable_data()[0] = (char)i;
            g_dummy_sink = pkt->data()[0];
        } });

    return 0;
}