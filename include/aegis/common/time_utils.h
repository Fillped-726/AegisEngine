// include/aegis/common/time_utils.h
#pragma once

#include <chrono> // [DEPENDENCY: std::chrono]
#include <cstdint>

namespace aegis::common
{
    // [INTENT: Monotonic timestamp generation via steady_clock; immune to NTP/OS adjustments]
    class TimeUtil
    {
    public:
        // [INTENT: Epoch offset in milliseconds]
        static inline uint64_t get_now_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        // [INTENT: Epoch offset in microseconds]
        static inline uint64_t get_now_us()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        // [INTENT: Epoch offset in nanoseconds]
        static inline uint64_t get_now_ns()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    };
} // namespace aegis::common