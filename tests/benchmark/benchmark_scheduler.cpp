#include <benchmark/benchmark.h>
#include "aegis/core/scheduler.h"
#include "aegis/core/actor.h"
#include "aegis/core/message.h"

using namespace aegis::core;

// 1. 模拟一个具体的业务 Actor
class PingPongActor : public Actor
{
public:
    PingPongActor() : Actor() {}

    // 实现基类纯虚函数：销毁逻辑
    void finalize() override
    {
        // 在调度器测试中，我们手动管理或让 Scheduler 自动销毁
        // 如果是从 ObjectPool 拿的，这里应该归还池
    }

    // 实现基类纯虚函数：消息处理逻辑
    void handle_message(ActorMessage *msg) override
    {
        // 模拟极其轻量的业务逻辑
        benchmark::DoNotOptimize(msg);
    }
};

// 2. 调度器开销测试：派发与执行
static void BM_Actor_Dispatch_Latency(benchmark::State &state)
{
    auto &sched = Scheduler::instance();

    // 初始化调度器（仅执行一次）
    if (state.thread_index() == 0)
    {
        sched.start(4); // 绑定 0-3 核
    }

    // 准备一个持久化的 Actor
    auto *actor = new PingPongActor();
    actor->set_id({1, 1}); // 模拟 ID

    for (auto _ : state)
    {
        // 模拟外部线程向 Actor 发送消息并触发调度
        auto *msg = new ActorMessage();     // 模拟普通消息
        msg->type_id = MSG_TYPE_SCENE_MOVE; // 命中 switch 中的普通分支

        // 核心动作：push 会触发 Actor 的 in_global_queue_ 状态翻转
        // dispatch 则将 Actor 送入 WorkStealing 流程
        if (actor->push(msg))
        {
            sched.dispatch(actor);
        }
    }

    // 清理
    state.SetItemsProcessed(state.iterations());

    // 注意：这里的销毁逻辑在真实压测中需要小心处理，防止 Double Free
}
// 强制多线程压测：模拟多个生产者向同一个 Actor 塞消息
BENCHMARK(BM_Actor_Dispatch_Latency)->Threads(1)->Threads(4)->UseRealTime();

BENCHMARK_MAIN();