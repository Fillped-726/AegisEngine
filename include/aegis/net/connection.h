#pragma once

#include <vector>
#include <deque>
#include <memory>
#include <atomic>
#include "aegis/core/task.h"
#include "aegis/net/socket.h"
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include "aegis/net/packet.h"
#include "aegis/net/packetPool.h" // 确保能看到 PooledPacket 定义
#include "aegis/common/spinLock.h"
#include "aegis/net/outbox_batcher.h"

namespace aegis::net
{

    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        // Constants
        static constexpr size_t K_INITIAL_RX_SIZE = 4096;
        static constexpr size_t K_SHRINK_THRESHOLD = 1 * 1024 * 1024; // 1MB 阈值触发收缩
        static constexpr size_t K_MAX_PACKET_SIZE = 10 * 1024 * 1024; // 10MB
        static constexpr size_t K_HEADER_SIZE = 4;

        explicit Connection(Socket &&s);
        ~Connection();

        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;

        [[nodiscard]] const Socket &socket() const { return socket_; }
        [[nodiscard]] int fd() const { return socket_.native_handle(); }

        // 读取逻辑
        core::Task<PooledPacket> read_packet();

        // 发送逻辑：接收智能指针，所有权转移给 Connection
        // 使用 PooledPacket (unique_ptr) 确保生命周期安全，自动归还对象池
        void send(PooledPacket packet);

        void flush();

    private:
        struct Outbox
        {
            std::vector<PooledPacket> buffer;
            aegis::common::SpinLock lock;
        };

        core::DetachedTask send_batch_coro(std::shared_ptr<Connection> self);

        // 内存管理辅助
        void ensure_rx_capacity(size_t required_size);
        void try_shrink_rx_buffer();

        std::atomic<bool> in_pending_queue_{false};
        std::atomic<bool> is_flushing_{false};

        Socket socket_;
        std::vector<char> rx_buffer_;
        size_t rx_len_ = 0;

        Outbox outbox_;
        OutboxBatcher batcher_;
    };

} // namespace aegis::net