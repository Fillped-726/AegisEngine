/**
 * @file connection.h
 * @brief Single-threaded async TCP peer with coroutine-based read and zero-copy writev send.
 */
#pragma once

#include <vector>
#include <deque>
#include <memory>
#include "aegis/core/task.h"  // [DEPENDENCY: aegis::core::Task, core::DetachedTask]
#include "aegis/net/socket.h" // [DEPENDENCY: aegis::net::Socket]
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include "aegis/net/packet.h"
#include "aegis/net/packetPool.h" // [DEPENDENCY: aegis::net::PooledPacket]
#include "aegis/net/outbox_batcher.h"

namespace aegis::net
{
    // [INTENT: Single-threaded async TCP peer state machine]
    // [STATE: Shared ownership lifecycle for safe coroutine capture]
    /**
     * @brief Single-threaded async TCP peer state machine.
     * 
     * Handles coroutine-based packet assembly (read_packet),
     * outbox queuing (send), and zero-copy writev flush (flush).
     * Owns Socket and OutboxBatcher for egress.
     * 
     * Shared ownership (enable_shared_from_this) ensures safe
     * coroutine capture across async boundaries.
     */
    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        // [STATE: IO memory tuning parameters]
        static constexpr size_t K_INITIAL_RX_SIZE = 4096;
        static constexpr size_t K_SHRINK_THRESHOLD = 1 * 1024 * 1024;
        static constexpr size_t K_MAX_PACKET_SIZE = 10 * 1024 * 1024;
        static constexpr size_t K_HEADER_SIZE = 4;

        // [STATE_MUTATION: Adopt connected peer socket]
        explicit Connection(Socket &&s);
        ~Connection();

        // [CONSTRAINT: Non-copyable to maintain strict resource ownership]
        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;

        [[nodiscard]] const Socket &socket() const { return socket_; }
        [[nodiscard]] int fd() const { return socket_.native_handle(); }

        // [INTENT: Coroutine suspension point yielding assembled protocol framing]
        core::Task<PooledPacket> read_packet();

        // [INTENT: Gracefully close connection]
        void close();

        // [STATE_MUTATION: Enqueue payload to egress buffer]
        void send(PooledPacket packet);

        // [STATE_MUTATION: Manually dispatch egress queue to OS]
        void flush();

    private:
        // [STATE: Lock-free, single-threaded egress payload queue]
        struct Outbox
        {
            std::vector<PooledPacket> buffer;
        };

        // [INTENT: Async egress IO multiplexing loop; detached execution]
        core::DetachedTask send_batch_coro(std::shared_ptr<Connection> self);

        // [STATE_MUTATION: Dynamic ingress buffer resizing]
        void ensure_rx_capacity(size_t required_size);
        void try_shrink_rx_buffer();

        // [STATE: Active egress IO loop guard; assumes single-thread context]
        bool is_flushing_{false};

        // [STATE: Egress backpressure limit]
        static constexpr size_t K_OUTBOX_LIMIT = 1024;

        // [STATE: Connection lifecycle guard]
        std::atomic<bool> closed_{false};

        // [STATE: Raw IO handle]
        Socket socket_;

        // [STATE: Ingress assembly byte stream]
        std::vector<char> rx_buffer_;
        size_t rx_len_ = 0;

        // [STATE: Egress subsystem]
        Outbox outbox_;
        OutboxBatcher batcher_;
    };

} // namespace aegis::net