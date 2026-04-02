#pragma once

#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

// [DEPENDENCY: CPU cache line topology constants]
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
#else
constexpr std::size_t hardware_constructive_interference_size = 64;
#endif

namespace aegis::common
{
    // [INTENT: Lock-free Chase-Lev work-stealing deque]
    template <typename T, size_t CAPACITY = 4096>
    class WorkStealingQueue
    {
        // [CONSTRAINT: Power-of-2 capacity for bitwise modulo masking]
        static_assert((CAPACITY & (CAPACITY - 1)) == 0, "Capacity must be power of 2");
        static constexpr size_t MASK = CAPACITY - 1;

        // [CONSTRAINT: Trivially copyable for atomic payload integrity]
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        static_assert(std::atomic<T>::is_always_lock_free, "T must be lock-free atomic compatible");

    public:
        WorkStealingQueue() : top_(0), bottom_(0)
        {
        }

        virtual ~WorkStealingQueue() = default;

        // [CONSTRAINT: Non-copyable/non-movable semantics]
        WorkStealingQueue(const WorkStealingQueue &) = delete;
        WorkStealingQueue &operator=(const WorkStealingQueue &) = delete;

        // [STATE_MUTATION: Owner LIFO push]
        bool push(T item)
        {
            size_t b = bottom_.load(std::memory_order_relaxed);

            // [PERF: Relaxed top_ load averts acquire penalty; tolerates false-full states]
            size_t t = top_.load(std::memory_order_relaxed);

            if (static_cast<int64_t>(b - t) >= static_cast<int64_t>(CAPACITY))
                return false;

            buffer_[b & MASK].store(item, std::memory_order_relaxed);

            bottom_.store(b + 1, std::memory_order_release);
            return true;
        }

        // [STATE_MUTATION: Owner LIFO pop]
        [[nodiscard]] std::optional<T> pop()
        {
            size_t b = bottom_.load(std::memory_order_relaxed);

            if (b == 0) [[unlikely]]
            {
                size_t t = top_.load(std::memory_order_relaxed);
                if (b <= t)
                    return std::nullopt;
            }

            b = b - 1;
            bottom_.store(b, std::memory_order_relaxed);

            // [INTENT: Sequential consistency fence prevents Store(bottom)-Load(top) reordering]
            std::atomic_thread_fence(std::memory_order_seq_cst);

            size_t t = top_.load(std::memory_order_relaxed);

            T item = buffer_[b & MASK].load(std::memory_order_relaxed);

            // [INTENT: Fast path; uncontended pop]
            if (static_cast<int64_t>(b - t) > 0)
            {
                return item;
            }

            // [INTENT: Queue empty; rollback bottom mutation]
            if (static_cast<int64_t>(b) < static_cast<int64_t>(t))
            {
                bottom_.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
            }

            // [STATE_MUTATION: Single item contention resolution via CAS against thief]
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                bottom_.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
            }

            bottom_.store(b + 1, std::memory_order_relaxed);
            return item;
        }

        // [STATE_MUTATION: Thief FIFO steal]
        [[nodiscard]] std::optional<T> steal()
        {
            size_t t = top_.load(std::memory_order_acquire);

            // [INTENT: Synchronization barrier mitigates false contention]
            std::atomic_thread_fence(std::memory_order_seq_cst);

            size_t b = bottom_.load(std::memory_order_acquire);

            if (static_cast<int64_t>(t - b) >= 0)
                return std::nullopt;

            T item = buffer_[t & MASK].load(std::memory_order_relaxed);

            // [STATE_MUTATION: Claim ownership via strict SeqCst CAS]
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed))
            {
                return std::nullopt;
            }

            return item;
        }

    private:
        // [STATE: Lock-free payload buffer]
        std::array<std::atomic<T>, CAPACITY> buffer_;

        // [PERF: Topology-aware alignment neutralizes destructive interference/false sharing]
        alignas(hardware_constructive_interference_size) std::atomic<size_t> top_;
        alignas(hardware_constructive_interference_size) std::atomic<size_t> bottom_;
    };
}