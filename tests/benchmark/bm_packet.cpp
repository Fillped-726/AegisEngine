#include <benchmark/benchmark.h>
#include "aegis/net/packet.h"
#include "aegis/net/packetPool.h"
#include <vector>

using namespace aegis::net;

// ==========================================
// 场景 A：SBO 命中 vs 堆分配 (纯 Packet 分配)
// ==========================================

// 模拟小包 (SBO 命中)
static void BM_Packet_SmallAlloc(benchmark::State &state)
{
    for (auto _ : state)
    {
        Packet p;
        p.alloc(512); // 小于 1024，不产生堆分配
        benchmark::DoNotOptimize(p.mutable_data());
    }
}
BENCHMARK(BM_Packet_SmallAlloc);

// 模拟大包 (触发 Heap Fallback)
static void BM_Packet_LargeAlloc(benchmark::State &state)
{
    for (auto _ : state)
    {
        Packet p;
        p.alloc(2048); // 触发 new[]
        benchmark::DoNotOptimize(p.mutable_data());
    }
}
BENCHMARK(BM_Packet_LargeAlloc);

// ==========================================
// 场景 B：PacketPool 威力 (对象复用)
// ==========================================

static void BM_PacketPool_Acquire(benchmark::State &state)
{
    for (auto _ : state)
    {
        auto p = PacketPool::instance().acquire();
        p->alloc(512);
        // p 在作用域结束时自动返回对象池 (Deleter 机制)
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_PacketPool_Acquire)->ThreadRange(1, 4)->UseRealTime();

// ==========================================
// 场景 C：移动语义开销 (Move vs Copy)
// ==========================================
static void BM_Packet_Move(benchmark::State &state)
{
    Packet src;
    src.alloc(512);
    for (auto _ : state)
    {
        Packet dest = std::move(src); // 触发 move_from
        src = std::move(dest);        // 移回来以便循环
        benchmark::DoNotOptimize(src);
    }
}
BENCHMARK(BM_Packet_Move);

BENCHMARK_MAIN();