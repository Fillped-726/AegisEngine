// include/aegis/common/spinLock.h
#pragma once

#include <atomic>
#include <thread>
#include <new>

// [DEPENDENCY: Architecture-specific CPU intrinsics]
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace aegis::common
{
    // [PERF: Detect L1 cache line size to prevent false sharing; fallback to 64 bytes]
#ifdef __cpp_lib_hardware_interference_size
    constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    constexpr std::size_t kCacheLineSize = 64;
#endif

    // [INTENT: CPU yield hint to optimize instruction pipeline and power during busy-wait]
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

    // [STATE: Lock mechanism padded to cache line boundary]
    struct alignas(kCacheLineSize) SpinLock
    {
    private:
        // [STATE: Atomic synchronization primitive]
        std::atomic_flag flag = ATOMIC_FLAG_INIT;

    public:
        SpinLock() noexcept = default;
        SpinLock(const SpinLock &) = delete;
        SpinLock &operator=(const SpinLock &) = delete;

        // [STATE_MUTATION: Block until acquisition]
        void lock() noexcept
        {
            // [INTENT: Fast path Test-and-Set (TAS)]
            if (!flag.test_and_set(std::memory_order_acquire))
            {
                return;
            }

            // [INTENT: Slow path Test-and-Test-and-Set (TTAS)]
            int spin_count = 0;
            while (true)
            {
                // [PERF: Relaxed read loop maintains L1 shared state; minimizes bus traffic]
                while (flag.test(std::memory_order_relaxed))
                {
                    cpu_relax();
                    spin_count++;
                }

                // [STATE_MUTATION: Re-attempt TAS acquisition]
                if (!flag.test_and_set(std::memory_order_acquire))
                {
                    return;
                }
            }
        }

        // [STATE_MUTATION: Relinquish control with memory barrier]
        void unlock() noexcept
        {
            flag.clear(std::memory_order_release);
        }

        // [INTENT: Non-blocking acquisition attempt]
        [[nodiscard]] bool try_lock() noexcept
        {
            return !flag.test_and_set(std::memory_order_acquire);
        }
    };
}