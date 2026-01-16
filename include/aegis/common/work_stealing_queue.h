#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <type_traits>
#include <cassert>

// C++17 硬件干扰大小
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
#else
constexpr std::size_t hardware_constructive_interference_size = 64;
#endif

namespace aegis::common
{

    /**
     * @brief 固定大小的环形窃取队列 (Ring Buffer Deque)
     * @details
     * 1. 支持 LIFO Pop (Owner) 和 FIFO Steal (Thief)。
     * 2. **非线程安全**：此类只负责数据结构操作，并发安全必须由外部锁（如 SpinLock）保证。
     * 3. 这里的内存序 (Acquire/Release) 是为了配合外部锁的临界区，防止指令重排溢出边界。
     * * @tparam Task 任务类型 (通常是指针)
     * @tparam CAPACITY 容量，必须是 2 的幂
     */
    template <typename Task, size_t CAPACITY = 4096>
    class WorkStealingQueue
    {
        static_assert((CAPACITY & (CAPACITY - 1)) == 0, "Capacity must be power of 2");
        static constexpr size_t MASK = CAPACITY - 1;

    public:
        WorkStealingQueue() : head_(0), tail_(0) {}

        // 禁止拷贝和赋值
        WorkStealingQueue(const WorkStealingQueue &) = delete;
        WorkStealingQueue &operator=(const WorkStealingQueue &) = delete;

        /**
         * @brief 放入任务 (队尾)
         * @note [Owner Thread Only] 必须持有锁
         */
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

        /**
         * @brief 弹出任务 (队尾 - LIFO)
         * @note [Owner Thread Only] 必须持有锁
         */
        [[nodiscard]] Task pop()
        {
            size_t t = tail_.load(std::memory_order_relaxed);
            size_t h = head_.load(std::memory_order_relaxed); // 锁内可见性由外部锁保证，这里 relaxed 即可

            if (h >= t)
                return Task{}; // Empty (对于 shared_ptr 是 nullptr)

            // 先减 tail，类似于 "预定" 这个位置
            size_t t_new = t - 1;
            tail_.store(t_new, std::memory_order_relaxed);

            return std::move(buffer_[t_new & MASK]);
        }

        /**
         * @brief 窃取任务 (队头 - FIFO)
         * @note [Thief Thread Only] 必须持有锁
         */
        [[nodiscard]] Task steal()
        {
            size_t h = head_.load(std::memory_order_relaxed);
            size_t t = tail_.load(std::memory_order_acquire); // 需要看到 push 的 release

            if (h >= t)
                return Task{}; // Empty

            Task task = std::move(buffer_[h & MASK]);
            head_.store(h + 1, std::memory_order_release); // 提交 head 的修改
            return task;
        }

        bool empty() const
        {
            return head_.load(std::memory_order_relaxed) >= tail_.load(std::memory_order_relaxed);
        }

        size_t size() const
        {
            size_t t = tail_.load(std::memory_order_relaxed);
            size_t h = head_.load(std::memory_order_relaxed);
            return (t >= h) ? (t - h) : 0;
        }

    private:
        std::array<Task, CAPACITY> buffer_;

        // 避免 False Sharing
        alignas(hardware_constructive_interference_size) std::atomic<size_t> head_;
        alignas(hardware_constructive_interference_size) std::atomic<size_t> tail_;
    };

} // namespace aegis::common