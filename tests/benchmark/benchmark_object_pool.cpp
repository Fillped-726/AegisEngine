#include <benchmark/benchmark.h>
#include "aegis/common/objectPool.h" // 假设你的文件名
#include <vector>

using namespace aegis::core;

// 测试对象：模拟一个中型对象，带有一些数据
struct MockObject
{
    uint64_t data[16]; // 128 bytes
    void reset() { data[0] = 0; }
};

// ==========================================
// 1. 基准测试：原生 new/delete (jemalloc 托管)
// ==========================================
static void BM_RawNewDelete(benchmark::State &state)
{
    for (auto _ : state)
    {
        auto *obj = new MockObject();
        benchmark::DoNotOptimize(obj);
        delete obj;
    }
}
BENCHMARK(BM_RawNewDelete)->ThreadRange(1, 4)->UseRealTime();

// ==========================================
// 2. 场景 A & B：ObjectPool 热路径 (L1 Cache)
// ==========================================
static void BM_ObjectPool_HotPath(benchmark::State &state)
{
    auto &pool = ObjectPool<MockObject>::instance();
    for (auto _ : state)
    {
        // 申请并立即释放，始终命中 ThreadLocalCache
        auto ptr = pool.acquire();
        benchmark::DoNotOptimize(ptr);
    }
}
// 测试单线程到 8 线程（验证 ThreadLocal 隔离性）
BENCHMARK(BM_ObjectPool_HotPath)->ThreadRange(1, 4)->UseRealTime();

// ==========================================
// 3. 场景 C：批量搬运 (Trigger L2 Global Queue)
// ==========================================
static void BM_ObjectPool_BulkTransfer(benchmark::State &state)
{
    auto &pool = ObjectPool<MockObject>::instance();
    const size_t batch_size = 200; // 超过默认 LocalBatchSize(128)

    for (auto _ : state)
    {
        std::vector<ObjectPool<MockObject>::Ptr> objs;
        objs.reserve(batch_size);

        // 1. 连续申请，强制触发从 Global Queue 拉取
        for (size_t i = 0; i < batch_size; ++i)
        {
            objs.push_back(pool.acquire());
        }

        // 2. 连续释放，强制触发推送到 Global Queue
        objs.clear();
    }
}
BENCHMARK(BM_ObjectPool_BulkTransfer)->ThreadRange(1, 4)->UseRealTime();

BENCHMARK_MAIN();