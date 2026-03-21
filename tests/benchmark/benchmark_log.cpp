#include <benchmark/benchmark.h>
#include "aegis/common/aegisLog.h" // 你的日志头文件
#include <spdlog/async.h>
#include <spdlog/sinks/null_sink.h> // 使用空槽，排除磁盘 IO 干扰

using namespace aegis;

// 初始化异步日志环境
void SetupAsyncLogger()
{
    // 预热单例
    auto &log = Log::instance();
    // 假设你在 init_config 里配置了异步模式
    // 为了公平测试，我们使用 null_sink，只测内存和逻辑损耗
    auto null_sink = std::make_shared<spdlog::sinks::null_sink_st>();

    // 设置 spdlog 线程池：8192个槽位，1个后台线程
    spdlog::init_thread_pool(8192, 1);
    auto async_logger = std::make_shared<spdlog::async_logger>(
        "aegis_async", null_sink, spdlog::thread_pool(), spdlog::async_overflow_policy::block);

    // 这里需要你 Log 类内部支持设置这个 logger_，或者通过 init_config 注入
    // 假设我们已经配置好了
}

// 场景 1：日志级别未达到，触发 Early Return
static void BM_LogDisabled(benchmark::State &state)
{
    Log::instance().set_level(spdlog::level::warn);
    for (auto _ : state)
    {
        // info 级别被屏蔽，测试 should_log 的开销
        Log::instance().info("This is a disabled log: {}", 42);
    }
}
BENCHMARK(BM_LogDisabled)->Threads(1)->Threads(4)->UseRealTime();

// 场景 2：正常异步日志输出（带格式化）
static void BM_LogAsync(benchmark::State &state)
{
    Log::instance().set_level(spdlog::level::info);
    int i = 0;
    for (auto _ : state)
    {
        Log::instance().info("Performance test log index: {}, some string: {}", ++i, "hello");
    }
}
BENCHMARK(BM_LogAsync)->Threads(1)->Threads(4)->UseRealTime();

BENCHMARK_MAIN();