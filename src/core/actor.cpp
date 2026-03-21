#include "aegis/core/actor.h"
#include "aegis/common/aegisLog.h" // 仅在 cpp 中包含日志
#include <exception>               // process_batch 捕获异常需要

namespace aegis::core
{
    // [Core] 线程局部存储
    // 每个 Worker 线程都会有自己的一份 t_current_actor
    static thread_local Actor *t_current_actor = nullptr;

    Actor::Actor(uint64_t parent_id)
        : parent_id_(parent_id)
    {
        // 初始状态：创建一个哑节点 (Stub)
        // 此时 Head 和 Tail 都指向它
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
        // head_ 始终指向"上一个已处理完的节点" (即当前的 Stub)
        // 真正的有效数据在 head_->next 中

        for (int i = 0; i < budget; ++i)
        {
            ActorMessage *head = head_; // 保存旧 Stub，稍后回收
            ActorMessage *next = head->next.load(std::memory_order_acquire);

            // --- 1. 队列判空逻辑 (完全保持原有无锁算法的精髓) ---
            if (next == nullptr)
            {
                in_global_queue_.store(false, std::memory_order_release);

                // Double Check: 处理 Race Condition
                ActorMessage *tail = tail_.load(std::memory_order_acquire);
                if (head != tail)
                {
                    bool expected = false;
                    if (in_global_queue_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                    {
                        continue; // 抢救回来，继续处理
                    }
                }
                return ActorState::Idle; // 真的空了
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

                    // 2. 注意：当前的 next (即 Destroy 消息本身) 现在变成了 head_ (新 Stub)
                    // 当调度器执行 delete actor 时，~Actor() 会遍历并清理链表，
                    // 所以这里不用手动 free(next)，交给析构函数处理剩余链表即可。

                    return ActorState::Dead; // <--- 唯一出口：通知 Scheduler 销毁我
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

        // Budget 用完了还有数据 (next != nullptr)，或者刚好处理完 budget 个
        // 保持 in_global_queue_ 为 true，让调度器重新入队
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
            // 为了防止彻底内存泄漏，这里可以尝试直接 delete msg
            // 虽然会 leak 派生类资源，但至少回收了基类内存。
            // 但在这个架构下，应该视作 Fatal Error。
            break;
        }
    }

} // namespace aegis::core