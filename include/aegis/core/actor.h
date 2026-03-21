#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <new>

// 仅保留必需的头文件，日志和异常等依赖移交 cpp
#include "aegis/net/packet.h"
#include "aegis/core/message.h"

// 适配不同编译器的缓存行大小获取
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
#else
#include <cstddef>
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
    class Actor
    {
    public:
        Actor(uint64_t parent_id = 0);
        virtual ~Actor();

        virtual void finalize() = 0;

        ActorID id() const { return id_; }
        ActorID parent_id() const { return parent_id_; }
        void set_id(ActorID id) { id_ = id; }
        void set_parent_id(ActorID parent_id) { parent_id_ = parent_id; }
        void base_reset(ActorID new_id, ActorID new_parent)
        {
            id_ = new_id;
            parent_id_ = new_parent;
        }

        // --- Producer API (Thread-Safe, Lock-Free) ---
        // 模板方法必须保留在头文件中
        template <typename T>
        bool push(T *msg)
        {
            static_assert(std::is_base_of<ActorMessage, T>::value, "Msg must derive from ActorMessage");

            // 1. 初始化新节点
            msg->next.store(nullptr, std::memory_order_relaxed);

            // 2. 原子交换 Tail (Serialization Point)
            ActorMessage *prev = tail_.exchange(msg, std::memory_order_acq_rel);

            // 3. 将旧队尾链接到新节点
            prev->next.store(msg, std::memory_order_release);

            // 4. 调度逻辑 (状态机翻转)
            bool expected = false;
            return in_global_queue_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
        }

        // --- Consumer API (Worker Thread Only) ---
        ActorState process_batch(int budget = 100);

        // --- 上下文管理 (Thread Local Context) ---
        static Actor *current();
        static void set_current(Actor *actor);

        static void free_message(ActorMessage *msg);

    protected:
        // 子类实现具体的业务逻辑
        virtual void handle_message(ActorMessage *msg) = 0;

    private:
        static constexpr size_t kCacheLine = hardware_constructive_interference_size;

        // Consumer 独占变量 (频繁读取/写入)
        alignas(kCacheLine) ActorMessage *head_;

        // Producer/Consumer 共享变量 (频繁写入 - 竞争热点)
        alignas(kCacheLine) std::atomic<ActorMessage *> tail_;

        // 调度状态
        alignas(kCacheLine) std::atomic<bool> in_global_queue_{false};

        // 父子关系
        ActorID id_;
        ActorID parent_id_;
    };
} // namespace aegis::core