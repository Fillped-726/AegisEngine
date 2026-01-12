#pragma once

#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <array>
#include <memory>

// Third-party
#include "concurrentqueue.h"

// Aegis
#include "aegis/core/actor.h"
#include "aegis/common/spinLock.h"

// C++17 硬件干扰大小 (通常是 64字节/Cache Line)
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
#else
constexpr std::size_t hardware_constructive_interference_size = 64;
#endif

// 定义任务类型
using SchedulerTask = std::shared_ptr<aegis::core::Actor>;

/**
 * @brief 基于自旋锁保护的环形窃取队列
 * @note 这是一个 Locked Queue，不是 Lock-free Chase-Lev Deque。
 * 牺牲了一点点并发度，换取了极高的正确性和实现的简单性。
 */
template <typename Task, size_t CAPACITY = 4096>
class WorkStealingQueue
{
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = CAPACITY - 1;

public:
    WorkStealingQueue() : head_(0), tail_(0) {}

    // [Owner Only] Push (Back)
    // 外部必须持有 SpinLock
    bool push(Task task)
    {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_acquire);

        // 检查满
        if (static_cast<int64_t>(t - h) >= static_cast<int64_t>(CAPACITY))
            return false;

        buffer_[t & MASK] = std::move(task);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // [Owner Only] Pop (Back - LIFO)
    // 外部必须持有 SpinLock
    Task pop()
    {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_relaxed);

        if (h >= t)
            return nullptr; // Empty

        size_t t_new = t - 1;
        tail_.store(t_new, std::memory_order_relaxed); // 锁内 relaxed 足够
        return std::move(buffer_[t_new & MASK]);
    }

    // [Thief Only] Steal (Front - FIFO)
    // 外部必须持有 SpinLock
    Task steal()
    {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);

        if (h >= t)
            return nullptr; // Empty

        Task task = std::move(buffer_[h & MASK]);
        head_.store(h + 1, std::memory_order_release);
        return task;
    }

    bool empty() const
    {
        return head_.load(std::memory_order_relaxed) >= tail_.load(std::memory_order_relaxed);
    }

    // 估算大小（无锁读取，仅供参考）
    size_t size() const
    {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_relaxed);
        return (t >= h) ? (t - h) : 0;
    }

private:
    std::array<Task, CAPACITY> buffer_;

    // 使用 alignas 避免伪共享 (False Sharing)
    alignas(hardware_constructive_interference_size) std::atomic<size_t> head_;
    alignas(hardware_constructive_interference_size) std::atomic<size_t> tail_;
};

namespace aegis::core
{
    class Scheduler
    {
    public:
        static Scheduler &instance();

        void start(int num_workers);
        void stop();

        /// @brief 核心调度接口
        /// @param task 待执行的 Actor
        /// - Worker 调用：放入本地 LIFO
        /// - 外部调用：放入全局 FIFO
        void dispatch(SchedulerTask task);

    private:
        Scheduler() = default;
        ~Scheduler();

        void worker_entry(int id);
        void notify_one_worker();

        std::optional<SchedulerTask> try_local_pop(int worker_id);
        std::optional<SchedulerTask> try_global_pop(int worker_id);
        std::optional<SchedulerTask> try_steal(int thief_id);

        void execute_actor(SchedulerTask task);

        struct alignas(hardware_constructive_interference_size) LocalQueue
        {
            WorkStealingQueue<SchedulerTask> q;
            common::SpinLock lock;
        };

        std::atomic<bool> running_{false};
        std::vector<std::thread> workers_;

        // 本地队列集合
        std::vector<std::unique_ptr<LocalQueue>> local_queues_;

        // 全局无锁队列 (MPMC)
        moodycamel::ConcurrentQueue<SchedulerTask> global_queue_;

        // 线程休眠/唤醒控制
        std::mutex sleep_mtx_;
        std::condition_variable sleep_cv_;
        std::atomic<int> sleeping_workers_{0};
    };

} // namespace aegis::core