#include <benchmark/benchmark.h>
#include "aegis/common/spinLock.h" // 假设你的文件名
#include <mutex>
#include <atomic>

using namespace aegis::common;

struct SharedData
{
    uint64_t counter = 0;
};

// ==========================================
// 1. std::mutex 对照组
// ==========================================
static void BM_Lock_Mutex(benchmark::State &state)
{
    static SharedData data;
    static std::mutex mtx;
    for (auto _ : state)
    {
        std::lock_guard lock(mtx);
        data.counter++;
    }
}
BENCHMARK(BM_Lock_Mutex)->Threads(1)->Threads(4)->UseRealTime();

// ==========================================
// 2. std::atomic 对照组 (硬核硬件指令)
// ==========================================
static void BM_Lock_Atomic(benchmark::State &state)
{
    static std::atomic<uint64_t> counter{0};
    for (auto _ : state)
    {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
}
BENCHMARK(BM_Lock_Atomic)->Threads(1)->Threads(4)->UseRealTime();

// ==========================================
// 3. Aegis SpinLock (TTAS + Yield)
// ==========================================
static void BM_Lock_SpinLock(benchmark::State &state)
{
    static SharedData data;
    static SpinLock spin;
    for (auto _ : state)
    {
        spin.lock();
        data.counter++;
        spin.unlock();
    }
}
BENCHMARK(BM_Lock_SpinLock)->Threads(1)->Threads(4)->UseRealTime();

BENCHMARK_MAIN();