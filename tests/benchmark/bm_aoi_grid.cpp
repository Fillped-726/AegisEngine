#include <benchmark/benchmark.h>
#include "aegis/game/aoi_grid.h"
#include <random>

using namespace aegis::core;

// 模拟 1000x1000 的地图，30x30 的格子大小
static AOIGrid g_grid(0.0f, 0.0f, 1000.0f, 1000.0f, 30.0f);

// ==========================================
// 场景 A：高频插入与删除 (Add/Remove)
// ==========================================
static void BM_AOI_AddRemove(benchmark::State &state)
{
    std::mt19937 gen(42);
    std::uniform_real_distribution<float> dist(0.0f, 999.0f);
    EntityId id = 1001;

    for (auto _ : state)
    {
        float x = dist(gen);
        float y = dist(gen);
        g_grid.Add(id, x, y);
        g_grid.Remove(id, x, y);
    }
}
BENCHMARK(BM_AOI_AddRemove)->Threads(1)->Threads(4)->UseRealTime();

// ==========================================
// 场景 B：九宫格视野查询 (GetViewEntityIds)
// ==========================================
static void BM_AOI_GetView(benchmark::State &state)
{
    // 预填一些数据
    for (int i = 0; i < 5000; ++i)
        g_grid.Add(i, i % 1000, i % 1000);

    std::vector<EntityId> result;
    result.reserve(128);

    for (auto _ : state)
    {
        // 查询中心区域
        g_grid.GetViewEntityIds(500.0f, 500.0f, result);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_AOI_GetView)->Threads(1)->Threads(4)->UseRealTime();

// ==========================================
// 场景 C：跨格移动与 Diff 计算 (Move)
// ==========================================
static void BM_AOI_MoveDiff(benchmark::State &state)
{
    std::vector<EntityId> enter, leave;
    EntityId id = 9999;

    for (auto _ : state)
    {
        // 从 (10, 10) 移动到 (45, 45)，假设跨越了格子
        g_grid.Move(id, 10.0f, 10.0f, 45.0f, 45.0f, enter, leave);
    }
}
BENCHMARK(BM_AOI_MoveDiff)->Threads(1)->Threads(4)->UseRealTime();

BENCHMARK_MAIN();