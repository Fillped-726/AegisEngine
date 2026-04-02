#include "aegis/common/actor_utils.h"
#include "aegis/core/actor.h"
#include "aegis/core/worker.h"
#include "aegis/core/scheduler.h"

namespace aegis::core
{
    void dispatch_msg(Actor *actor, ActorMessage *msg)
    {
        if (!actor || !msg)
            return;

        // 1. 尝试压入队列并翻转状态机
        if (actor->push(msg))
        {
            // 2. 获取该 Actor 归属的 Worker
            auto *target_worker = Scheduler::instance().get_worker(actor->worker_id());

            // 3. 调度决策
            if (Worker::get_current_id() == actor->worker_id())
            {
                // 如果当前就在目标线程，直接塞进本地无锁队列 (Fast Path)
                target_worker->dispatch_local(actor);
            }
            else
            {
                // 跨核投递，内部必须触发 eventfd 唤醒目标线程 (Slow Path)
                target_worker->post_cross_core_task(actor);
            }
        }
    }
}