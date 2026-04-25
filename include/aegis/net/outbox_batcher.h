/**
 * @file outbox_batcher.h
 * @brief Zero-copy scatter-gather writev view builder with partial write support.
 */
#pragma once

#include <vector>
#include <deque>
#include <sys/uio.h>   // [DEPENDENCY: POSIX sys/uio (iovec)]
#include <arpa/inet.h> // [DEPENDENCY: POSIX arpa/inet (htonl)]
#include <limits.h>
#include "aegis/net/packet.h"
#include "aegis/net/packetPool.h"

namespace aegis::net
{
    // [INTENT: Zero-copy scatter-gather (writev) view builder; manages transient header lifecycle]
    /**
     * @brief Zero-copy writev view builder for batched egress.
     * 
     * Constructs iovec arrays from Packet queue, prepending big-endian
     * frame headers. Supports partial write (advance()) via cursor tracking.
     * Max batch: 64 packets (IOV_MAX-safe guard).
     */
    class OutboxBatcher
    {
    public:
        // [CONSTRAINT: Bound syscall latency and respect OS IOV_MAX]
        static constexpr size_t BATCH_LIMIT = 64;

        // [PERF: Preallocate vector capacity to eliminate runtime reallocation jitter]
        OutboxBatcher()
        {
            iovecs_.reserve(BATCH_LIMIT * 2);
            header_cache_.reserve(BATCH_LIMIT);
        }

        // [STATE_MUTATION: Hydrate iovec array from packet queue]
        size_t prepare_batch(const std::vector<PooledPacket> &queue)
        {
            iovecs_.clear();
            header_cache_.clear();
            packet_end_indices_.clear();

            consumed_iov_index_ = 0;
            completed_packets_cursor_ = 0;
            size_t count = 0;

            for (const auto &pkt : queue)
            {
                // [CONSTRAINT: Hard limit on single-syscall iovec count]
                if (count >= BATCH_LIMIT || iovecs_.size() + 2 > 1024)
                {
                    break;
                }

                uint32_t body_len = static_cast<uint32_t>(pkt->size());
                uint32_t net_len = htonl(body_len);

                // [STATE: Materialize Big-Endian header into heap to guarantee memory lifecycle across syscall]
                header_cache_.push_back(net_len);

                struct iovec iov_h;
                iov_h.iov_base = &header_cache_.back();
                iov_h.iov_len = sizeof(uint32_t);
                iovecs_.push_back(iov_h);

                if (body_len > 0)
                {
                    struct iovec iov_b;
                    // [INTENT: Zero-copy payload mapping]
                    iov_b.iov_base = const_cast<char *>(pkt->data());
                    iov_b.iov_len = body_len;
                    iovecs_.push_back(iov_b);
                }

                packet_end_indices_.push_back(iovecs_.size());
                count++;
            }

            return count;
        }

        // [STATE_MUTATION: Fast-forward internal cursors based on actual bytes written]
        size_t advance(size_t written_bytes)
        {
            size_t packets_completed = 0;

            while (written_bytes > 0 && consumed_iov_index_ < iovecs_.size())
            {
                struct iovec &iov = iovecs_[consumed_iov_index_];

                if (written_bytes >= iov.iov_len)
                {
                    // [INTENT: Segment fully consumed]
                    written_bytes -= iov.iov_len;
                    consumed_iov_index_++;
                }
                else
                {
                    // [INTENT: Partial write resolution; mutate pending iovec base/len]
                    iov.iov_base = static_cast<char *>(iov.iov_base) + written_bytes;
                    iov.iov_len -= written_bytes;
                    written_bytes = 0;
                }

                // [INTENT: Map consumed iovecs back to completed logical packets]
                while (completed_packets_cursor_ < packet_end_indices_.size() &&
                       consumed_iov_index_ >= packet_end_indices_[completed_packets_cursor_])
                {
                    packets_completed++;
                    completed_packets_cursor_++;
                }
            }

            return packets_completed;
        }

        const struct iovec *iov_data() const { return iovecs_.data(); }
        int iov_count() const { return static_cast<int>(iovecs_.size()); }

        // [INTENT: Aggregate pending byte count]
        size_t total_bytes() const
        {
            size_t bytes = 0;
            for (const auto &iov : iovecs_)
            {
                bytes += iov.iov_len;
            }
            return bytes;
        }

        // [INTENT: Expose unconsumed scatter-gather span]
        std::span<struct iovec> remaining_iovecs()
        {
            if (consumed_iov_index_ >= iovecs_.size())
                return {};
            return {iovecs_.data() + consumed_iov_index_, iovecs_.size() - consumed_iov_index_};
        }

        bool is_empty() const { return consumed_iov_index_ >= iovecs_.size(); }

    private:
        // [STATE: Syscall payload structure]
        std::vector<struct iovec> iovecs_;
        // [STATE: Backing store for protocol headers lacking existing memory addresses]
        std::vector<uint32_t> header_cache_;
        // [STATE: Monotonically increasing iovec boundaries per packet]
        std::vector<size_t> packet_end_indices_;
        // [STATE: Read heads for advancing partial writes]
        size_t consumed_iov_index_ = 0;
        size_t completed_packets_cursor_ = 0;
    };
}