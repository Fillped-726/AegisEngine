#pragma once

#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

// ---------------------------------------------------------------------------
// 平台兼容性：Cache Line 大小定义
// ---------------------------------------------------------------------------
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
#else
// 64 bytes on x86-64, 128 bytes on ARM64 (Apple Silicon)
constexpr std::size_t hardware_constructive_interference_size = 64;
#endif

namespace aegis::common
{
    /**
     * @brief 高性能无锁工作窃取队列 (Chase-Lev Deque)
     * @tparam T 存储的数据类型。
     * 【最佳实践】T 应当是指针 (e.g. Job*) 或轻量级句柄。
     * 要求 T 必须是 Trivially Copyable，否则无法进行无锁拷贝。
     * @tparam CAPACITY 队列容量，必须是 2 的幂。
     */
    template <typename T, size_t CAPACITY = 4096>
    class WorkStealingQueue
    {
        // -----------------------------------------------------------------------
        // 静态断言 (Static Asserts) - 编译期熔断保护
        // -----------------------------------------------------------------------
        static_assert((CAPACITY & (CAPACITY - 1)) == 0, "Capacity must be power of 2");
        static constexpr size_t MASK = CAPACITY - 1;

        // 确保 T 是"平凡可拷贝"的，防止 std::atomic 使用内部锁
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        static_assert(std::atomic<T>::is_always_lock_free, "T must be lock-free atomic compatible");

    public:
        WorkStealingQueue() : top_(0), bottom_(0)
        {
        }

        virtual ~WorkStealingQueue() = default;

        // 禁止拷贝和移动 (该数据结构天然不支持)
        WorkStealingQueue(const WorkStealingQueue &) = delete;
        WorkStealingQueue &operator=(const WorkStealingQueue &) = delete;

        /**
         * @brief 入队 (仅 Owner 线程调用)
         * @return true 成功, false 队列已满
         */
        bool push(T item)
        {
            size_t b = bottom_.load(std::memory_order_relaxed);

            // [优化] 使用 relaxed 读取 top
            // 即使读到旧值导致"虚假满"，也比 acquire 省开销。
            size_t t = top_.load(std::memory_order_relaxed);

            if (static_cast<int64_t>(b - t) >= static_cast<int64_t>(CAPACITY))
                return false;

            // 写入 buffer
            buffer_[b & MASK].store(item, std::memory_order_relaxed);

            // 发布新 bottom
            bottom_.store(b + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief 出队 (仅 Owner 线程调用)
         * @return std::nullopt 若队列空或竞争失败
         */
        [[nodiscard]] std::optional<T> pop()
        {
            size_t b = bottom_.load(std::memory_order_relaxed);

            // 快速下溢检查 (Underflow Check)
            if (b == 0) [[unlikely]]
            {
                // 此时 b=0, 如果 t=0, 则是空。
                // 必须重新 load top 确认，防止初始状态误判
                size_t t = top_.load(std::memory_order_relaxed);
                if (b <= t)
                    return std::nullopt;
            }

            // 预先减 1 (Optimistic decrement)
            b = b - 1;
            bottom_.store(b, std::memory_order_relaxed);

            // [关键] SeqCst Fence
            // 防止 Store(bottom) 重排到 Load(top) 之后导致 Double Pop
            std::atomic_thread_fence(std::memory_order_seq_cst);

            size_t t = top_.load(std::memory_order_relaxed);

            // 预读取数据 (无副作用拷贝)
            T item = buffer_[b & MASK].load(std::memory_order_relaxed);

            // Case 1: 任务充足 (Fast Path)
            if (static_cast<int64_t>(b - t) > 0)
            {
                return item;
            }

            // Case 2: 队列已空
            if (static_cast<int64_t>(b) < static_cast<int64_t>(t))
            {
                bottom_.store(b + 1, std::memory_order_relaxed); // 恢复 bottom
                return std::nullopt;
            }

            // Case 3: 竞争 (b == t) - 最后一个元素
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                // CAS 失败：被 Thief 偷走了
                bottom_.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
            }

            // CAS 成功：Owner 胜出
            bottom_.store(b + 1, std::memory_order_relaxed);
            return item;
        }

        /**
         * @brief 窃取 (任意线程调用)
         * @return std::nullopt 若窃取失败
         */
        [[nodiscard]] std::optional<T> steal()
        {
            // 确保看到其他 Thief 修改后的最新 top，减少无意义的 CAS 尝试
            size_t t = top_.load(std::memory_order_acquire);

            // [关键] SeqCst Fence
            // 防止虚假竞争 (False Contention)
            std::atomic_thread_fence(std::memory_order_seq_cst);

            size_t b = bottom_.load(std::memory_order_acquire);

            // 检查是否为空
            if (static_cast<int64_t>(t - b) >= 0)
                return std::nullopt;

            // 乐观读取 (Optimistic Read)
            T item = buffer_[t & MASK].load(std::memory_order_relaxed);

            // CAS 竞争所有权
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                return std::nullopt; // 失败
            }

            return item; // 成功
        }

    private:
        // 数据存储
        std::array<std::atomic<T>, CAPACITY> buffer_;

        // 核心索引 (使用 alignas 防止伪共享)
        // 内存布局：| top (64B) | bottom (64B) |
        alignas(hardware_constructive_interference_size) std::atomic<size_t> top_;
        alignas(hardware_constructive_interference_size) std::atomic<size_t> bottom_;
    };
}
