#pragma once

#include <atomic>
#include <new>    // for std::hardware_destructive_interference_size
#include <thread> // for yield context if needed (optional)

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace aegis::common
{
    constexpr std::size_t kCacheLineSize = 64;

    /**
     * @brief CPU 弛豫指令封装
     * 在自旋等待期间提示 CPU 流水线，避免过度发热并优化超线程性能
     */
    inline void cpu_relax() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#else
        // Fallback for other archs
#endif
    }

    /**
     * @brief 高性能自旋锁 (SpinLock) - TTAS + Cache Aligned
     * * @note [MVP Status]
     * 1. 强制 Cache Line 对齐，彻底解决 False Sharing。
     * 2. 采用 TTAS (Test-Test-And-Set) 策略，配合 C++20 atomic_flag::test()。
     * 3. 适用于锁持有时间极短（纳秒/微秒级）的场景。
     */
    struct alignas(kCacheLineSize) SpinLock
    {
        // C++20 保证默认构造为 clear 状态
        std::atomic_flag flag = ATOMIC_FLAG_INIT;

        SpinLock() noexcept = default;

        // 严格禁止拷贝和移动（锁的语义决定了它必须锚定在内存地址）
        SpinLock(const SpinLock &) = delete;
        SpinLock &operator=(const SpinLock &) = delete;
        SpinLock(SpinLock &&) = delete;
        SpinLock &operator=(SpinLock &&) = delete;

        /**
         * @brief 获取锁 (TTAS 策略)
         * * 优化原理：
         * 1. 先进行 relaxed load (test)，此时 cache line 处于 Shared (S) 状态。
         * 2. 只有当发现锁空闲时，才尝试 RMW (Read-Modify-Write)，请求 Exclusive (E/M) 状态。
         * 3. 避免了在锁被占用时，多个 CPU 核心频繁争抢总线写权限导致的 "Bus Storm"。
         */
        void lock() noexcept
        {
            while (true)
            {
                // 阶段 1: 乐观尝试获取锁 (Test-And-Set)
                // memory_order_acquire 保证临界区内存读写不会重排到加锁前
                if (!flag.test_and_set(std::memory_order_acquire))
                {
                    return;
                }

                // 阶段 2: 自旋等待 (Test Loop)
                // 使用 memory_order_relaxed，仅观察值，不产生同步副作用，减少开销
                while (flag.test(std::memory_order_relaxed))
                {
                    cpu_relax();
                }
            }
        }

        void unlock() noexcept
        {
            // memory_order_release 保证临界区内存读写全部完成
            flag.clear(std::memory_order_release);
        }

        /**
         * @brief 尝试获取锁 (Non-blocking)
         * 适用于 Work-Stealing 场景
         * @return true 获取成功, false 获取失败
         */
        [[nodiscard]] bool try_lock() noexcept
        {
            // 对于 try_lock，直接 TAS 即可，不需要自旋等待
            return !flag.test_and_set(std::memory_order_acquire);
        }
    };
} // namespace aegis::common