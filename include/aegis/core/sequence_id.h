/**
 * @file sequence_id.h
 * @brief Per-worker atomic sequence ID generator.
 *
 * Format: uint32_t = (worker_id << 24) | (counter & 0xFFFFFF)
 *   - High 8 bits:  Worker ID (0-255)
 *   - Low 24 bits:   Monotonically incrementing counter
 *
 * Thread-local worker_id is used so no cross-thread synchronization needed.
 * Counter wraps around at 24 bits safely; old requests with the same SeqID
 * will have already timed out before wrap occurs.
 */
#pragma once

#include <cstdint>
#include <atomic>

namespace aegis::core
{

    class SequenceIDGen
    {
    public:
        explicit SequenceIDGen(uint32_t worker_id)
            : worker_id_(worker_id & 0xFF) {}

        /**
         * @brief Generate a new globally unique SeqID.
         *
         * Thread-safe without external locking because each Worker has its own
         * SequenceIDGen instance.
         */
        uint32_t next()
        {
            uint32_t counter = counter_.fetch_add(1, std::memory_order_relaxed) & 0xFFFFFF;
            return (worker_id_ << 24) | counter;
        }

        /**
         * @brief Extract worker_id from a SeqID.
         */
        static inline uint32_t extract_worker(uint32_t seq_id)
        {
            return (seq_id >> 24) & 0xFF;
        }

        /**
         * @brief Extract counter portion from a SeqID.
         */
        static inline uint32_t extract_counter(uint32_t seq_id)
        {
            return seq_id & 0xFFFFFF;
        }

    private:
        uint32_t worker_id_;
        std::atomic<uint32_t> counter_{0};
    };

} // namespace aegis::core
