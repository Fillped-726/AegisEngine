#pragma once

#include <atomic>
#include <array>
#include <optional>
#include <cassert>

// SSP 标配：防止 False Sharing
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
#else
constexpr std::size_t hardware_constructive_interference_size = 64;
#endif

namespace aegis::common
{
    /**
     * @brief 真正的 Lock-Free Work-Stealing Queue (基于 Chase-Lev 算法)
     * @details
     * - Owner: push (bottom), pop (bottom) -> 几乎无锁 (仅 atomic store/load)
     * - Thief: steal (top) -> CAS 竞争
     */
    template <typename Task, size_t CAPACITY = 4096>
    class WorkStealingQueue
    {
        // 容量必须是 2 的幂，以便使用位运算取模
        static_assert((CAPACITY & (CAPACITY - 1)) == 0, "Capacity must be power of 2");
        static constexpr size_t MASK = CAPACITY - 1;

    public:
        WorkStealingQueue() : top_(0), bottom_(0) {}

        // 1. Push (仅 Owner 调用) - 几乎无开销
        bool push(Task task)
        {
            size_t b = bottom_.load(std::memory_order_relaxed);
            size_t t = top_.load(std::memory_order_acquire);

            if (static_cast<int64_t>(b - t) >= static_cast<int64_t>(CAPACITY))
                return false;

            buffer_[b & MASK] = std::move(task);

            // 关键：Release 屏障，保证 task 写入在 bottom 更新之前对其他线程可见
            std::atomic_thread_fence(std::memory_order_release);

            bottom_.store(b + 1, std::memory_order_relaxed);
            return true;
        }

        // 2. Pop (仅 Owner 调用) - 只有在队列空或冲突时才需要 CAS
        std::optional<Task> pop()
        {
            size_t b = bottom_.load(std::memory_order_relaxed);

            // 队列已空
            if (b == 0)
                return std::nullopt; // 防止下溢

            b = b - 1;
            bottom_.store(b, std::memory_order_relaxed);

            // 关键：SeqCst 屏障。这是 Chase-Lev 算法最微妙的地方。
            // 它保证 Owner 看到 top 的最新值，同时也保证 Thieves 看到 bottom 的最新值。
            // 必须在 decrement bottom 之后，load top 之前。
            std::atomic_thread_fence(std::memory_order_seq_cst);

            size_t t = top_.load(std::memory_order_relaxed);

            // Case 1: 队列里有不止一个任务，或者刚好有一个且没冲突
            if (static_cast<int64_t>(b) > static_cast<int64_t>(t))
            {
                return std::move(buffer_[b & MASK]);
            }

            // Case 2: 队列空了 (Bottom 追上了 Top)
            if (b < t)
            {
                bottom_.store(b + 1, std::memory_order_relaxed); // 恢复 bottom
                return std::nullopt;
            }

            // Case 3: 竞争！(b == t)
            // 此时队列里只剩最后一个任务。Owner 和 Thief 可能同时在抢。
            // 我们必须尝试修改 Top 来决胜负。
            Task task = std::move(buffer_[b & MASK]);

            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                // CAS 失败：被 Thief 偷走了
                bottom_.store(b + 1, std::memory_order_relaxed); // 恢复 bottom
                return std::nullopt;
            }

            // CAS 成功：Owner 抢到了最后一个任务
            bottom_.store(b + 1, std::memory_order_relaxed); // 恢复 bottom (Chase-Lev 的特性，pop 后 bottom 也要复位)
            return task;
        }

        // 3. Steal (任意线程调用) - 必须 CAS
        std::optional<Task> steal()
        {
            size_t t = top_.load(std::memory_order_acquire);

            // 必须用 SeqCst fence 配合 Owner 的 pop，确保看到 bottom 的变化
            std::atomic_thread_fence(std::memory_order_seq_cst);

            size_t b = bottom_.load(std::memory_order_acquire);

            if (static_cast<int64_t>(t) >= static_cast<int64_t>(b))
                return std::nullopt; // 空

            Task task = std::move(buffer_[t & MASK]); // 乐观读取

            // 尝试推进 top
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                // CAS 失败：说明 Owner 正在 pop 最后一个，或者被其他 Thief 偷了
                return std::nullopt;
            }

            return task;
        }

    private:
        // 环形缓冲区 (不用 vector 避免动态分配)
        std::array<Task, CAPACITY> buffer_;

        // 防止 False Sharing
        alignas(hardware_constructive_interference_size) std::atomic<size_t> top_;
        alignas(hardware_constructive_interference_size) std::atomic<size_t> bottom_;
    };
}