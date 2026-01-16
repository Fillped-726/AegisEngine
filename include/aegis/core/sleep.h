#pragma once

#include <coroutine>
#include <memory>
#include "aegis/core/actor.h"
#include "aegis/core/hierarchy_timer.h"
#include "aegis/core/scheduler.h" // 需要引用 Scheduler 来重新调度 Actor

namespace aegis::core
{
    struct sleep
    {
        uint32_t delay_ms_;

        explicit sleep(uint32_t ms) : delay_ms_(ms) {}

        bool await_ready() const { return false; }

        void await_suspend(std::coroutine_handle<> h)
        {
            // 1. 计算 tick
            uint32_t ticks = (delay_ms_ + 9) / 10;
            if (ticks == 0)
                ticks = 1;

            // 2. 获取当前环境 (谁在调用 sleep?)
            // 如果是在 Worker 线程中运行的 Actor，这里会返回非空
            Actor *owner_ptr = Actor::current();

            // 3. 尝试获取强引用 (Shared Ptr)
            // 这是一个保护措施，防止定时器触发时 Actor 已经被销毁
            std::shared_ptr<Actor> owner;
            if (owner_ptr)
            {
                owner = owner_ptr->shared_from_this();
            }

            // 4. 注册定时器
            // 注意：这个 Lambda 会被拷贝到堆上，作为 TimerNode 的一部分
            HierarchicalTimeWheel::instance().add_timer(ticks, [h, owner]() mutable
                                                        {
                
                // --- 以下代码将在 IO 线程 (Timer 线程) 执行 ---

                if (owner)
                {
                    // [Case A: Actor 环境]
                    // 我们不能在 IO 线程直接 resume，必须把协程送回 Actor
                    
                    // 1. 构造唤醒消息
                    auto msg = new CoroutineWakeupMsg(h);
                    
                    // 2. 投递到 Actor 的无锁邮箱 (Thread-Safe)
                    owner->push(msg);
                    
                    // 3. 告诉 Scheduler 这个 Actor 有新消息了，需要被调度执行
                    // 这样 Worker 线程稍后会取出这个 Actor，处理 WakeupMsg，并在那里 resume
                    Scheduler::instance().dispatch(owner);
                }
                else
                {
                    // [Case B: 裸环境] (例如 main 函数里的测试协程)
                    // 没有 Actor 依附，直接原地恢复
                    // 风险提示：这意味着协程剩下的代码会在 IO 线程跑
                    h.resume();
                } });
        }

        void await_resume() const noexcept {}
    };

} // namespace aegis::core