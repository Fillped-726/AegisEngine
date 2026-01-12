#include "aegis/core/scheduler.h"
#include "aegis/core/actor.h"
#include "aegis/common/aegisLog.h"
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <thread> // [修复] 必须包含，否则 hardware_concurrency 和 yield 报错
#include <memory> // [修复] 必须包含，否则 unique_ptr/shared_ptr 报错

using namespace aegis::core;

// --- 简单的消息池，避免 malloc 干扰调度器测试 ---
struct BenchMsg : public ActorMessage
{
    int val = 0;
};

class MsgPool
{
public:
    MsgPool(size_t size)
    {
        ptrs_.reserve(size);
        for (size_t i = 0; i < size; ++i)
        {
            // 直接 new 裸指针，不使用 unique_ptr 管理
            // 因为这些消息最终会被 Engine delete
            ptrs_.push_back(new BenchMsg());
        }
    }

    // 析构时不做任何事，因为指针已经交出去了
    ~MsgPool() = default;

    BenchMsg *alloc()
    {
        if (ptrs_.empty())
        {
            // 备用方案：池子空了就现场 new
            return new BenchMsg();
        }
        BenchMsg *m = ptrs_.back();
        ptrs_.pop_back();
        return m;
    }

private:
    std::vector<BenchMsg *> ptrs_;
};

// --- 测试用的 Actor ---
class BenchActor : public Actor
{
public:
    BenchActor(std::atomic<size_t> &counter) : global_counter_(counter) {}

protected:
    // [修复] 去掉未使用的参数名，消除 -Wunused-parameter 警告
    void handle_message(ActorMessage * /*msg*/) override
    {
        // 模拟业务负载 (极其轻量，纯测调度开销)
        // 稍微加一点计算防止编译器过度优化
        volatile int x = 0;
        for (int i = 0; i < 10; ++i)
            x = x + 1; // [修复] C++20 废弃了 volatile 的 ++ 操作

        // 计数器减一
        global_counter_.fetch_sub(1, std::memory_order_release);
    }

private:
    std::atomic<size_t> &global_counter_;
};

// --- 场景 1: 外部高并发注入 (测试 Global Queue + Stealing) ---
void bench_flood(int num_workers, int num_actors, int total_msgs)
{
    std::cout << "\n[Scenario 1: The Flood (Global Injection)]" << std::endl;
    std::cout << "Workers: " << num_workers << ", Actors: " << num_actors << ", Msgs: " << total_msgs << std::endl;

    Scheduler::instance().start(num_workers);

    std::atomic<size_t> counter{(size_t)total_msgs};
    std::vector<std::shared_ptr<BenchActor>> actors;
    actors.reserve(num_actors);
    for (int i = 0; i < num_actors; ++i)
    {
        actors.push_back(std::make_shared<BenchActor>(counter));
    }

    // 预分配消息，避免测试 malloc
    MsgPool pool(total_msgs);

    auto start = std::chrono::high_resolution_clock::now();

    // 模拟 IO 线程疯狂 Dispatch
    // 将消息均匀分配给 Actor，瞬间塞满全局队列
    for (int i = 0; i < total_msgs; ++i)
    {
        auto msg = pool.alloc();
        // msg->type_id = 1; // ActorMessage 如果没有 type_id 成员请注释掉

        // 轮询分配给 Actor
        auto &actor = actors[i % num_actors];

        // 假设 Actor::push 返回 true 代表入队成功
        // 注意：如果 Scheduler::dispatch 需要 Actor*，请确保 actor.get() 正确
        // 这里的逻辑假设是：向 Actor 塞消息，然后把 Actor 扔进调度器
        if (actor->push(msg))
        {
            Scheduler::instance().dispatch(actor);
        }
    }

    // 等待所有任务完成
    while (counter.load(std::memory_order_acquire) > 0)
    {
        std::this_thread::yield();
    }

    auto end = std::chrono::high_resolution_clock::now();
    Scheduler::instance().stop();

    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    double qps = total_msgs / (ms / 1000.0);

    std::cout << "Time: " << ms << " ms" << std::endl;
    std::cout << "QPS : " << (size_t)qps << " ops/sec" << std::endl;
}

// --- 场景 2: 链式反应 (测试 Local Queue + LIFO) ---
// Actor 处理完消息后，立刻给自己发一条新消息，触发 Worker 本地队列优化
class ChainActor : public Actor, public std::enable_shared_from_this<ChainActor>
{
public:
    ChainActor(std::atomic<size_t> &counter, int limit)
        : global_counter_(counter), limit_(limit) {}

protected:
    void handle_message(ActorMessage * /*msg*/) override
    {
        global_counter_.fetch_sub(1, std::memory_order_release);
        processed_++;

        if (processed_ < limit_)
        {
            // 自发自收：这会触发 Scheduler::dispatch 的 "Worker内部调用" 分支
            // 应该放入 Local Queue，并在下一次循环优先被执行 (LIFO)
            auto new_msg = new BenchMsg(); // 链式测试为了模拟真实场景，这里允许 new，或者使用 thread_local pool
            if (this->push(new_msg))
            {
                Scheduler::instance().dispatch(shared_from_this());
            }
        }
    }

private:
    std::atomic<size_t> &global_counter_;
    int limit_;
    int processed_ = 0;
};

void bench_chain(int num_workers, int num_chains, int msgs_per_chain)
{
    size_t total_msgs = num_chains * msgs_per_chain;
    std::cout << "\n[Scenario 2: The Chain (Local LIFO Optimization)]" << std::endl;
    std::cout << "Workers: " << num_workers << ", Chains: " << num_chains << ", Msgs/Chain: " << msgs_per_chain << std::endl;

    Scheduler::instance().start(num_workers);

    std::atomic<size_t> counter{total_msgs};
    std::vector<std::shared_ptr<ChainActor>> actors;

    // 初始化 N 条链
    for (int i = 0; i < num_chains; ++i)
    {
        actors.push_back(std::make_shared<ChainActor>(counter, msgs_per_chain));
    }

    auto start = std::chrono::high_resolution_clock::now();

    // 触发第一张多米诺骨牌
    for (auto &actor : actors)
    {
        auto msg = new BenchMsg();
        if (actor->push(msg))
        {
            Scheduler::instance().dispatch(actor);
        }
    }

    while (counter.load(std::memory_order_acquire) > 0)
    {
        std::this_thread::yield();
    }

    auto end = std::chrono::high_resolution_clock::now();
    Scheduler::instance().stop();

    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    double qps = total_msgs / (ms / 1000.0);

    std::cout << "Time: " << ms << " ms" << std::endl;
    std::cout << "QPS : " << (size_t)qps << " ops/sec" << std::endl;
}

int main(int argc, char **argv)
{
    // 初始化日志 (只打印 WARN 以上，避免刷屏影响性能)
    // 假设 AegisLog 已经改成单例模式
    aegis::Log::instance().set_level(spdlog::level::warn);

    int workers = std::thread::hardware_concurrency();
    if (argc > 1)
        workers = std::atoi(argv[1]);

    // 防止 workers 为 0
    if (workers == 0)
        workers = 4;

    std::cout << "=== Aegis Scheduler Stress Test ===" << std::endl;
    std::cout << "Hardware Cores: " << std::thread::hardware_concurrency() << std::endl;
    std::cout << "Target Workers: " << workers << std::endl;

    // 1. 洪峰测试：1000 Actor，共处理 500万条消息
    // 考察点：Global Queue 吞吐, Lock contention, False sharing
    // 增加消息量以获得更稳定的结果
    bench_flood(workers, 1000, 5000000);

    // 2. 链式测试：1000 Actor，每个自循环 5000 次
    // 考察点：Local Queue LIFO, Cache locality, Scheduler overhead
    bench_chain(workers, 1000, 5000);

    return 0;
}