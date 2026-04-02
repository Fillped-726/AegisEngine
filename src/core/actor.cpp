#include "aegis/core/actor.h"
#include "aegis/common/aegisLog.h" // 仅在 cpp 中包含日志
#include <exception>               // process_batch 捕获异常需要
#include "aegis/core/worker.h"     // 定时器调度需要访问 Worker 的时间轮

namespace aegis::core
{
    // [Core] 线程局部存储
    // 每个 Worker 线程都会有自己的一份 t_current_actor
    static thread_local Actor *t_current_actor = nullptr;

    Actor::Actor(uint64_t parent_id)
        : parent_id_(parent_id)
    {
        ActorMessage *stub = new ActorMessage();
        stub->next.store(nullptr, std::memory_order_relaxed);

        head_ = stub;
        tail_.store(stub, std::memory_order_relaxed);
    }

    Actor::~Actor()
    {
        // 析构时，清理链表上残留的所有节点
        // 注意：因为采用了"延迟回收"，此时 head_ 指向的节点也需要被回收
        ActorMessage *curr = head_;
        while (curr)
        {
            ActorMessage *next = curr->next.load(std::memory_order_relaxed);
            free_message(curr); // 自定义回收
            curr = next;
        }
    }

    ActorState Actor::process_batch(int budget)
    {

        for (int i = 0; i < budget; ++i)
        {
            ActorMessage *head = head_; // 保存旧 Stub，稍后回收
            ActorMessage *next = head->next.load(std::memory_order_acquire);

            if (next == nullptr)
            {
                // Double Check: 判断是真没数据了，还是 Producer 被卡在了指令缝隙里
                ActorMessage *tail = tail_.load(std::memory_order_acquire);
                if (head == tail)
                {
                    // 【情况 A】：真没数据了。安全挂起。
                    is_scheduled_.store(false, std::memory_order_release);

                    // 防御性再检查一次，防止在 store(false) 的瞬间有新数据进来
                    // 这是为了彻底杜绝 Lost Wakeup
                    if (tail_.load(std::memory_order_acquire) != head)
                    {
                        bool expected = false;
                        if (is_scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                        {
                            // 抢救成功，下一轮继续
                            // 因为没有消费，这里退还 budget
                            --i;
                            continue;
                        }
                    }
                    return ActorState::Idle;
                }
                return ActorState::Active;
            }

            // --- 2. 节点步进 ---
            // next 成为新的 Stub
            head_ = next;

            try
            {
                switch (next->type_id)
                {
                // [Case A] 毒丸消息：立即终止
                case MSG_TYPE_DESTROY:
                {
                    // 1. 回收旧 Stub (head)
                    free_message(head);

                    return ActorState::Dead; // <--- 唯一出口：通知 worker 销毁我
                }

                // [Case B] 协程唤醒：基础设施
                case MSG_TYPE_CORO_WAKEUP:
                {
                    auto *wake_msg = static_cast<CoroutineWakeupMsg *>(next);
                    if (wake_msg->handle)
                    {
                        wake_msg->handle.resume();
                    }
                    break;
                }

                // [Case C] 普通业务消息：多态分发
                default:
                {
                    handle_message(next);
                    break;
                }
                }
            }
            catch (const std::exception &e)
            {
                aegis::Log::instance().error("Actor exception: {}", e.what());
            }
            catch (...)
            {
                aegis::Log::instance().error("Actor unknown exception");
            }

            // --- 4. 资源回收 ---
            // 回收旧的 Stub (head)
            // 此时 next 已经安全地变成了新的 head_
            Actor::free_message(head);
        }

        return ActorState::Active;
    }

    Actor *Actor::current()
    {
        return t_current_actor;
    }

    void Actor::set_current(Actor *actor)
    {
        t_current_actor = actor;
    }

    void Actor::free_message(ActorMessage *msg)
    {
        if (!msg)
            return;

        // 编译器会将这个 Switch 优化为 Jump Table (O(1) 跳转)
        // 性能极高，不要为了"代码好看"换成函数指针 Map，那样会有 Cache Miss 风险
        switch (msg->type_id)
        {
        // --- 基础消息 ---
        case MSG_TYPE_NETWORK:
            // NetworkMessage 是池化的，处理特殊
            static_cast<NetworkMessage *>(msg)->finalize();
            break;

        // --- 模板化消息 ---
        // 下面这些生成的汇编指令几乎一模一样，但必须写出来以便编译器生成对应的析构调用
        case MSG_TYPE_CORO_WAKEUP:
            static_cast<CoroutineWakeupMsg *>(msg)->finalize();
            break;
        case MSG_TYPE_SESSION_CLOSED:
            static_cast<SessionClosedMsg *>(msg)->finalize();
            break;
        case MSG_TYPE_SCENE_ENTER:
            static_cast<SceneEnterMsg *>(msg)->finalize();
            break;
        case MSG_TYPE_SCENE_LEAVE:
            static_cast<SceneLeaveMsg *>(msg)->finalize();
            break;
        case MSG_TYPE_SCENE_MOVE:
            static_cast<SceneMoveMsg *>(msg)->finalize();
            break;
        case MSG_TYPE_FORWARD_PACKET:
            static_cast<ForwardPacketMsg *>(msg)->finalize();
            break;

        // --- RPC 消息 ---
        // 这里必须 Cast 成对应的模板实例化类型
        case MSG_TYPE_RPC_CREATE_ROOM:
            static_cast<RPCCreateRoomMsg *>(msg)->finalize();
            break;
        case MSG_TYPE_RPC_TERMINATE_ROOM:
            static_cast<RPCTerminateRoomMsg *>(msg)->finalize();
            break;

        case MSG_TYPE_BASE:
            delete msg;
            break;
        case MSG_TYPE_DESTROY:
            // 毒丸通常由 Actor 内部逻辑处理，如果流转到了 free_message，说明是被丢弃的
            delete static_cast<ActorDestroyMsg *>(msg);
            break;

        default:
            aegis::Log::instance().error("Leak warning: Unknown message type in free_message: {}", msg->type_id);
            break;
        }
    }

    TimerId Actor::schedule_timer(uint32_t delay_ms, std::function<void()> cb)
    {
        return aegis::core::t_current_worker->time_wheel().add_timer(delay_ms, std::move(cb));
    }

} // namespace aegis::core