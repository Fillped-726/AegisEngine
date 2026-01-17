#include "aegis/core/scheduler.h"
#include "aegis/core/actor.h"
#include "aegis/common/aegisLog.h"
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>

using namespace aegis::core;

// --- 简单的消息池 ---
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
            ptrs_.push_back(new BenchMsg());
    }
    ~MsgPool()
    {
        for (auto p : ptrs_)
            delete p;
    } // [优化] 清理未使用的消息

    BenchMsg *alloc()
    {
        if (ptrs_.empty())
            return new BenchMsg();
        BenchMsg *m = ptrs_.back();
        ptrs_.pop_back();
        return m;
    }

private:
    std::vector<BenchMsg *> ptrs_;
};

// --- 场景 1: 测试 Actor ---
class BenchActor : public Actor
{
public:
    BenchActor(std::atomic<size_t> &counter) : global_counter_(counter) {}

protected:
    void handle_message(ActorMessage *msg) override
    {
        // [SSP 技巧] 检查是否是毒丸消息
        // 假设 ActorDestroyMsg 的类型识别逻辑如下
        // if (msg->get_type() == ActorMessage::Type::Destroy) { ... }

        // 模拟业务
        volatile int x = 0;
        for (int i = 0; i < 10; ++i)
            x = x + 1;

        global_counter_.fetch_sub(1, std::memory_order_release);
    }

private:
    std::atomic<size_t> &global_counter_;
};

// --- 场景 1 逻辑修复 ---
void bench_flood(int num_workers, int num_actors, int total_msgs)
{
    std::cout << "\n[Scenario 1: The Flood (Global Injection)]" << std::endl;
    Scheduler::instance().start(num_workers);

    std::atomic<size_t> counter{(size_t)total_msgs};

    // 1. 改为裸指针存储
    std::vector<BenchActor *> actors;
    actors.reserve(num_actors);
    for (int i = 0; i < num_actors; ++i)
    {
        actors.push_back(new BenchActor(counter));
    }

    MsgPool pool(total_msgs);
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < total_msgs; ++i)
    {
        auto msg = pool.alloc();
        auto actor = actors[i % num_actors];
        if (actor->push(msg))
        {
            // [修复] 传入裸指针
            Scheduler::instance().dispatch(actor);
        }
    }

    while (counter.load(std::memory_order_acquire) > 0)
        std::this_thread::yield();

    auto end = std::chrono::high_resolution_clock::now();

    // [关键] 发送毒丸消息销毁 Actor
    for (auto actor : actors)
    {
        // 假设你的 Actor 内部收到 ActorDestroyMsg 后会执行 delete this
        if (actor->push(new ActorDestroyMsg()))
        {
            Scheduler::instance().dispatch(actor);
        }
    }

    Scheduler::instance().stop();

    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "Time: " << ms << " ms | QPS : " << (size_t)(total_msgs / (ms / 1000.0)) << std::endl;
}

// --- 场景 2: 链式反应 ---
class ChainActor : public Actor
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
            auto new_msg = new BenchMsg();
            if (this->push(new_msg))
            {
                // [修复] 直接传 this，不再使用 shared_from_this()
                Scheduler::instance().dispatch(this);
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

    Scheduler::instance().start(num_workers);
    std::atomic<size_t> counter{total_msgs};

    // 1. 改为裸指针
    std::vector<ChainActor *> actors;
    for (int i = 0; i < num_chains; ++i)
    {
        actors.push_back(new ChainActor(counter, msgs_per_chain));
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (auto actor : actors)
    {
        if (actor->push(new BenchMsg()))
        {
            Scheduler::instance().dispatch(actor);
        }
    }

    while (counter.load(std::memory_order_acquire) > 0)
        std::this_thread::yield();
    auto end = std::chrono::high_resolution_clock::now();

    // [关键] 毒丸清理
    for (auto actor : actors)
    {
        if (actor->push(new ActorDestroyMsg()))
        {
            Scheduler::instance().dispatch(actor);
        }
    }

    Scheduler::instance().stop();

    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "Time: " << ms << " ms | QPS : " << (size_t)(total_msgs / (ms / 1000.0)) << std::endl;
}

int main(int argc, char **argv)
{
    aegis::Log::instance().set_level(spdlog::level::warn);
    int workers = (argc > 1) ? std::atoi(argv[1]) : (int)std::thread::hardware_concurrency();
    if (workers <= 0)
        workers = 4;

    bench_flood(workers, 1000, 5000000);
    bench_chain(workers, 1000, 5000);

    return 0;
}