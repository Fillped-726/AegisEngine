#pragma once

#include <atomic>
#include <memory>
#include <new>
#include <utility>
#include <type_traits>
#include <exception>

#include "aegis/net/packet.h"
#include "aegis/common/aegisLog.h" // 引入日志以记录异常
#include "aegis/core/message.h"

// 适配不同编译器的缓存行大小获取
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
#else
constexpr std::size_t hardware_constructive_interference_size = 64;
#endif

namespace aegis::core
{
    enum class ActorState
    {
        Active, // 还有任务，继续入队 (Reschedule)
        Idle,   // 没任务了，暂时移出队列 (Suspend)
        Dead    // 【关键】我已经自杀，请立即释放内存 (Deallocate)
    };
    // --- 3. 核心 Actor 引擎 (MPSC Lock-Free) ---
    /**
     * @brief 基于 Intrusive MPSC Queue 的 Actor 基类
     * * 实现采用了 "Stub Node" (哑节点) 机制：
     * - 队列中永远至少有一个节点 (Stub)。
     * - Head 指向当前 Stub，Tail 指向最后一个节点。
     * - Push 时更新 Tail。
     * - Pop 时，Head->next 才是真正的第一个数据节点。
     * - 处理完 Head->next 后，原来的 Head (旧 Stub) 被回收，
     * Head->next 变成新的 Stub (即它里面的数据被消费了，壳留着用作 Stub)。
     */
    class Actor
    {
    public:
        Actor()
        {
            // 初始状态：创建一个哑节点 (Stub)
            // 此时 Head 和 Tail 都指向它
            ActorMessage *stub = new ActorMessage();
            stub->next.store(nullptr, std::memory_order_relaxed);

            head_ = stub;
            tail_.store(stub, std::memory_order_relaxed);
        }

        virtual ~Actor()
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

        // --- Producer API (Thread-Safe, Lock-Free) ---
        // 任意线程调用
        // 返回 true 表示该 Actor 之前是空闲的 (inactive)，调度器需要将其放入队列
        template <typename T>
        bool push(T *msg)
        {
            static_assert(std::is_base_of<ActorMessage, T>::value, "Msg must derive from ActorMessage");

            // 1. 初始化新节点
            msg->next.store(nullptr, std::memory_order_relaxed);

            // 2. 原子交换 Tail (Serialization Point)
            // prev 是交换前的 tail，也就是当前的队尾
            ActorMessage *prev = tail_.exchange(msg, std::memory_order_acq_rel);

            // 3. 将旧队尾链接到新节点
            // 此时 Consumer 可能会顺着 prev->next 摸过来
            prev->next.store(msg, std::memory_order_release);

            // 4. 调度逻辑 (状态机翻转)
            // 只有当 in_global_queue 从 false 变 true 时，返回 true
            // 这保证了同一个 Actor 不会被重复加入调度队列
            bool expected = false;
            return in_global_queue_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
        }

        // --- Consumer API (Worker Thread Only) ---
        // 执行一批消息
        // budget: 时间片预算，防止单个 Actor 饿死其他 Actor
        // --- Consumer API (Worker Thread Only) ---
        ActorState process_batch(int budget = 100)
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

                // --- 3. 消息分发 (Switch-Case 架构层拦截) ---
                // 使用 switch 替代 if-else，利用跳转表优化，O(1) 复杂度
                bool should_continue = true;

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
                free_message(head);
            }

            // Budget 用完了还有数据 (next != nullptr)，或者刚好处理完 budget 个
            // 保持 in_global_queue_ 为 true，让调度器重新入队
            return ActorState::Active;
        }

        // --- 上下文管理 (Thread Local Context) ---
        // 允许 sleep() 知道自己属于哪个 Actor
        static Actor *current();
        static void set_current(Actor *actor);

    protected:
        // 子类实现具体的业务逻辑
        virtual void handle_message(ActorMessage *msg) = 0;

        // [C++20 优化版] 高性能消息回收器
        // 无虚函数调用，无 vptr 开销，完全静态分发
        void free_message(ActorMessage *msg)
        {
            if (!msg)
                return;

            // 这里利用 Switch 跳转表 + 静态转换
            // 编译器会生成极其高效的汇编代码，通常只有几条指令
            switch (msg->type_id)
            {
            case MSG_TYPE_BASE:
                msg->finalize(); // 直接调用基类的 finalize
                break;
            case MSG_TYPE_NETWORK:
                // static_cast 是编译期动作，零运行时开销
                // finalize() 是非虚函数，直接 inline 展开
                static_cast<NetworkMessage *>(msg)->finalize();
                break;
            case MSG_TYPE_CORO_WAKEUP:
                static_cast<CoroutineWakeupMsg *>(msg)->finalize();
                break;

            case MSG_TYPE_SESSION_CLOSED:
                static_cast<SessionClosedMsg *>(msg)->finalize();
                break;

            case MSG_TYPE_SCENE_ENTER:
                static_cast<SceneEnterMsg *>(msg)->finalize();
                break;
            case MSG_TYPE_SCENE_MOVE:
                static_cast<SceneMoveMsg *>(msg)->finalize();
                break;
            case MSG_TYPE_SCENE_LEAVE:
                static_cast<SceneLeaveMsg *>(msg)->finalize();
                break;
            case MSG_TYPE_DESTROY:
                // 特殊情况：毒丸消息交给析构函数处理
                // 不在这里 finalize
                break;
            default:
                aegis::Log::instance().error("Unknown message type in free_message: {}", msg->type_id);
                break;
            }
        }

    private:
        static constexpr size_t kCacheLine = hardware_constructive_interference_size;

        // Consumer 独占变量 (频繁读取/写入)
        alignas(kCacheLine) ActorMessage *head_;

        // Producer/Consumer 共享变量 (频繁写入 - 竞争热点)
        alignas(kCacheLine) std::atomic<ActorMessage *> tail_;

        // 调度状态
        alignas(kCacheLine) std::atomic<bool> in_global_queue_{false};
    };
} // namespace aegis::core