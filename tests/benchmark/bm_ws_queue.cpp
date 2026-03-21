#include <benchmark/benchmark.h>
#include "aegis/common/work_stealing_queue.h"
#include <deque>
#include <mutex>
#include <optional>

using namespace aegis::common;

// ==========================================
// 对照组：传统的 Mutex + Deque
// ==========================================
template <typename T>
class MutexQueue
{
    std::deque<T> queue_;
    std::mutex mtx_;

public:
    void push(T item)
    {
        std::lock_guard lock(mtx_);
        queue_.push_back(item);
    }
    std::optional<T> pop()
    {
        std::lock_guard lock(mtx_);
        if (queue_.empty())
            return std::nullopt;
        T item = queue_.back();
        queue_.pop_back();
        return item;
    }
    std::optional<T> steal()
    {
        std::lock_guard lock(mtx_);
        if (queue_.empty())
            return std::nullopt;
        T item = queue_.front();
        queue_.pop_front();
        return item;
    }
};

// ==========================================
// 场景 A：Owner 线程的 Push/Pop 吞吐量 (SPSC 路径)
// ==========================================
static void BM_SPSC_Mutex(benchmark::State &state)
{
    MutexQueue<int *> q;
    int *dummy = reinterpret_cast<int *>(0xDEADC0DE);
    for (auto _ : state)
    {
        q.push(dummy);
        benchmark::DoNotOptimize(q.pop());
    }
}
BENCHMARK(BM_SPSC_Mutex)->UseRealTime();

static void BM_SPSC_WorkStealing(benchmark::State &state)
{
    WorkStealingQueue<int *, 4096> q;
    int *dummy = reinterpret_cast<int *>(0xDEADC0DE);
    for (auto _ : state)
    {
        q.push(dummy);
        benchmark::DoNotOptimize(q.pop());
    }
}
BENCHMARK(BM_SPSC_WorkStealing)->UseRealTime();

// ==========================================
// 场景 B：1 个 Owner Push，N 个 Thief Steal
// ==========================================
static void BM_StealEfficiency(benchmark::State &state)
{
    static WorkStealingQueue<int *, 16384> q;
    int *dummy = reinterpret_cast<int *>(0xDEADC0DE);

    if (state.thread_index() == 0)
    {
        // Owner 线程：狂塞任务
        for (auto _ : state)
        {
            q.push(dummy);
        }
    }
    else
    {
        // Thief 线程：狂偷任务
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(q.steal());
        }
    }
}
// 测试 1个 Owner + 1, 2, 3 个 Thief (总共不超过 4 核)
BENCHMARK(BM_StealEfficiency)->ThreadRange(2, 4)->UseRealTime();

static MutexQueue<int *> global_mutex_q;

static void BM_Stress_Mutex_Centralized(benchmark::State &state)
{
    int *dummy = reinterpret_cast<int *>(0xDEADC0DE);
    for (auto _ : state)
    {
        // 所有线程都在抢这一把锁
        global_mutex_q.push(dummy);
        benchmark::DoNotOptimize(global_mutex_q.pop());
    }
}
// 强制 4 线程在 0-3 核竞争
BENCHMARK(BM_Stress_Mutex_Centralized)->Threads(4)->UseRealTime();

// ---------------------------------------------------------------------------
// 场景 B：去中心化的 WorkStealing (各自为政)
// ---------------------------------------------------------------------------
// 每个线程分配一个私有队列，防止伪共享
static WorkStealingQueue<int *, 4096> per_thread_queues[4];

static void BM_Stress_WorkStealing_Decentralized(benchmark::State &state)
{
    int *dummy = reinterpret_cast<int *>(0xDEADC0DE);
    // 获取当前线程对应的私有队列
    auto &my_queue = per_thread_queues[state.thread_index()];

    for (auto _ : state)
    {
        // 绝大多数时间在操作私有队列，无竞争
        my_queue.push(dummy);
        benchmark::DoNotOptimize(my_queue.pop());
    }
}
BENCHMARK(BM_Stress_WorkStealing_Decentralized)->Threads(4)->UseRealTime();

BENCHMARK_MAIN();