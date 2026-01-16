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
    class Actor : public std::enable_shared_from_this<Actor>
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
        bool process_batch(int budget = 100)
        {
            // head_ 始终指向"上一个已处理完的节点" (即当前的 Stub)
            // 真正的有效数据在 head_->next 中

            for (int i = 0; i < budget; ++i)
            {
                ActorMessage *head = head_;
                ActorMessage *next = head->next.load(std::memory_order_acquire);

                // 如果 next 为空，说明队列可能空了
                if (next == nullptr)
                {
                    // 标记为非活跃状态
                    in_global_queue_.store(false, std::memory_order_release);

                    // Double Check: 再次检查 Tail
                    // 这是为了处理"判空后瞬间又有数据推入"的 Race Condition
                    ActorMessage *tail = tail_.load(std::memory_order_acquire);

                    if (head != tail)
                    {
                        // 确实有新数据进来了 (tail 变了)，但 next 还是 null。
                        // 说明 Producer 刚刚执行完 exchange，还没来得及执行 prev->next = msg。
                        // 此时我们处于"中间态"。

                        // 尝试重新获取调度权
                        bool expected = false;
                        if (in_global_queue_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                        {
                            // 抢回调度权，继续自旋等待 Producer 完成链接
                            continue;
                        }
                    }
                    return false; // 真的空了，且已标记为 inactive，退出
                }

                // --- 核心逻辑：节点步进 ---
                // 'next' 是包含数据的节点。
                // 我们将 'head_' 移动到 'next'，使其成为新的 Stub。
                // 此时，原来的 'head' (即上一个 Stub) 可以安全删除了。
                head_ = next;

                // --- 业务执行区 (Exception Safe) ---
                try
                {
                    // [New] 拦截系统消息 (协程唤醒)
                    if (next->type_id == MSG_ID_CORO_WAKEUP)
                    {
                        auto *wake_msg = static_cast<CoroutineWakeupMsg *>(next);
                        if (wake_msg->handle)
                        {
                            // 在 Worker 线程恢复协程
                            // 此时上下文 (Actor::current) 已经在 Worker 中被设置好了
                            wake_msg->handle.resume();
                        }
                    }
                    else
                    {
                        // 普通业务消息，交给子类处理
                        handle_message(next);
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

                // --- 资源回收区 ---
                // 回收旧的 Stub (head)
                // 注意：我们绝不回收 next！因为 next 现在是 head_，是下一个节点的 Stub！
                // 这就是"延迟回收"的精髓
                free_message(head);
            }

            // Budget 用完了还有数据，保持 in_global_queue_ 为 true
            return true;
        }

        // --- 上下文管理 (Thread Local Context) ---
        // 允许 sleep() 知道自己属于哪个 Actor
        static Actor *current();
        static void set_current(Actor *actor);

    protected:
        // 子类实现具体的业务逻辑
        virtual void handle_message(ActorMessage *msg) = 0;

        // 封装删除逻辑，方便后续接入 Object Pool
        void free_message(ActorMessage *msg)
        {
            if (!msg)
                return;

            // [MVP TODO] 建议未来改为 switch 或虚函数表（如果内存允许）
            if (msg->type_id == 1) // NetworkMessage
            {
                delete static_cast<NetworkMessage *>(msg);
            }
            else if (msg->type_id == MSG_ID_CORO_WAKEUP) // [New] CoroutineWakeupMsg
            {
                delete static_cast<CoroutineWakeupMsg *>(msg);
            }
            else
            {
                delete msg; // System message (base)
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