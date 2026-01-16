#pragma once

#include <atomic>
#include <thread>
#include <new>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace aegis::common
{
    // 获取硬件造成的破坏性干扰大小（通常即 Cache Line 大小），如果编译器不支持则回退到 64
#ifdef __cpp_lib_hardware_interference_size
    constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    constexpr std::size_t kCacheLineSize = 64;
#endif

    inline void cpu_relax() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#else
        // fallback
#endif
    }

    struct alignas(kCacheLineSize) SpinLock
    {
    private:
        std::atomic_flag flag = ATOMIC_FLAG_INIT;

        static constexpr int kMaxSpinsBeforeYield = 4000;

    public:
        SpinLock() noexcept = default;
        SpinLock(const SpinLock &) = delete;
        SpinLock &operator=(const SpinLock &) = delete;

        void lock() noexcept
        {
            // 快速路径：如果运气好，一次就拿到了，完全不用进循环
            if (!flag.test_and_set(std::memory_order_acquire))
            {
                return;
            }

            // 慢速路径：开始自旋
            int spin_count = 0;
            while (true)
            {
                // Inner Loop: 只读自旋 (TTAS 的第一个 T)
                // 在这个循环里，cache line 处于 Shared 状态，不产生总线流量
                while (flag.test(std::memory_order_relaxed))
                {
                    if (spin_count < kMaxSpinsBeforeYield)
                    {
                        cpu_relax();
                        spin_count++;
                    }
                    else
                    {
                        // 惩罚机制：自旋太久了，说明锁竞争激烈或持有者被切走了
                        // 主动让出 CPU，防止活锁 (Livelock)
                        std::this_thread::yield();
                        spin_count = 0; // 归零，回来后继续尝试自旋
                    }
                }

                // 尝试获取锁 (TTAS 的 TAS)
                // 只有上面的循环检测到锁释放了，这里才会执行原子写
                if (!flag.test_and_set(std::memory_order_acquire))
                {
                    return; // 成功拿到锁
                }

                // 如果 CAS 失败（被别人抢了），回到大循环继续 read-spin
            }
        }

        void unlock() noexcept
        {
            flag.clear(std::memory_order_release);
        }

        [[nodiscard]] bool try_lock() noexcept
        {
            return !flag.test_and_set(std::memory_order_acquire);
        }
    };
}