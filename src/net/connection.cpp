#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"
#include <cstring>
#include <arpa/inet.h>
#include <algorithm>

struct WriteContext
{
    std::vector<aegis::net::PooledPacket> packets; // 保活 Packet
};

namespace aegis::net
{

    Connection::Connection(Socket &&s)
        : socket_(std::move(s))
    {
        rx_buffer_.resize(K_INITIAL_RX_SIZE);
    }

    Connection::~Connection()
    {
        // Outbox 中的 PooledPacket 会在此处析构，自动归还给 PacketPool，无泄露。
        aegis::Log::instance().debug("Connection destroyed. FD: {}", socket_.native_handle());
    }

    void Connection::ensure_rx_capacity(size_t required_size)
    {
        if (rx_buffer_.size() < required_size)
        {
            // 扩容策略：至少翻倍，避免频繁 realloc
            size_t new_size = std::max(rx_buffer_.size() * 2, required_size);
            rx_buffer_.resize(new_size);
        }
    }

    // [New Feature] 缓冲区收缩策略
    void Connection::try_shrink_rx_buffer()
    {
        // 如果容量大于 1MB 且 当前使用量小于 4KB (利用率极低)
        if (rx_buffer_.capacity() > K_SHRINK_THRESHOLD && rx_len_ < K_INITIAL_RX_SIZE)
        {
            aegis::Log::instance().debug("Shrinking RX buffer from {} to {}", rx_buffer_.capacity(), K_INITIAL_RX_SIZE);

            std::vector<char> new_buffer(K_INITIAL_RX_SIZE);
            if (rx_len_ > 0)
            {
                std::memcpy(new_buffer.data(), rx_buffer_.data(), rx_len_);
            }
            // Swap 之后，巨大的旧 buffer 会随着 new_buffer (现在的临时变量) 的析构而释放
            rx_buffer_.swap(new_buffer);
        }
    }

    core::Task<PooledPacket> Connection::read_packet()
    {
        try
        {
            auto packet = net::PacketPool::instance().acquire();
            // 1. Read Header
            while (rx_len_ < K_HEADER_SIZE)
            {
                if (rx_len_ == rx_buffer_.size())
                    rx_buffer_.resize(rx_buffer_.size() * 2);

                int n = co_await socket_.recv(rx_buffer_.data() + rx_len_, rx_buffer_.size() - rx_len_);
                if (n <= 0)
                    co_return nullptr;
                rx_len_ += n;
            }

            // 2. Parse Length
            uint32_t net_len;
            std::memcpy(&net_len, rx_buffer_.data(), K_HEADER_SIZE);
            uint32_t body_len = ntohl(net_len);
            uint32_t total_len = K_HEADER_SIZE + body_len;

            if (total_len > K_MAX_PACKET_SIZE)
            {
                aegis::Log::instance().error("Packet too large: {}", total_len);
                co_return nullptr;
            }

            // 3. Read Body
            ensure_rx_capacity(total_len);
            while (rx_len_ < total_len)
            {
                int n = co_await socket_.recv(rx_buffer_.data() + rx_len_, rx_buffer_.size() - rx_len_);
                if (n <= 0)
                    co_return nullptr;
                rx_len_ += n;
            }

            // 4. Extract Data
            packet->alloc(body_len);
            if (body_len > 0)
            {
                std::memcpy(packet->mutable_data(), rx_buffer_.data() + K_HEADER_SIZE, body_len);
            }

            // 5. Handle Stash (Move remaining data to front)
            size_t remaining = rx_len_ - total_len;
            if (remaining > 0)
            {
                std::memmove(rx_buffer_.data(), rx_buffer_.data() + total_len, remaining);
            }
            rx_len_ = remaining;

            // [New] 尝试收缩缓冲区
            // 放在包处理完毕、数据移到最前端之后是最佳时机
            try_shrink_rx_buffer();

            co_return packet;
        }
        catch (const std::exception &e)
        {
            aegis::Log::instance().error("Read error: {}", e.what());
            co_return nullptr;
        }
    }

    // [Refactor] 接收 PooledPacket (unique_ptr)
    void Connection::send(PooledPacket packet)
    {
        if (!packet)
            return;

        outbox_.buffer.push_back(std::move(packet));

        bool expected = false;
        if (!is_flushing_)
        {
            flush();
        }
    }

    void Connection::flush()
    {

        if (is_flushing_)
        {
            return; // 已经有协程在后台干活了，撤退
        }

        is_flushing_ = true;

        send_batch_coro(shared_from_this());
    }

    core::DetachedTask Connection::send_batch_coro(std::shared_ptr<Connection> self)
    {
        std::vector<net::PooledPacket> batch;

        while (true)
        {
            // 【极速无锁化】干掉所有的 lock_guard
            if (self->outbox_.buffer.empty())
            {
                // 没有数据了，修改普通 bool 标志，结束协程
                self->is_flushing_ = false;
                co_return;
            }

            // 直接 swap，零锁开销
            batch.swap(self->outbox_.buffer);

            // 1. 准备 iovecs
            size_t count = self->batcher_.prepare_batch(batch);

            while (!self->batcher_.is_empty())
            {
                auto iovs = self->batcher_.remaining_iovecs();
                int res = -1;
                try
                {
                    // 【暗流涌动】：这里的 socket_.send 底层现在会通过 TLS 获取当前 Worker 的 io_uring
                    // 然后挂起当前协程，将控制权交还回 Worker 的 Event Loop
                    res = co_await self->socket_.send(iovs);
                }
                catch (const std::exception &e)
                {
                    aegis::Log::instance().error("Async send exception: {}", e.what());
                    res = -1;
                }

                if (res < 0)
                {
                    // Fatal error
                    self->socket_.close();
                    self->is_flushing_ = false;
                    co_return;
                }

                // Advance the batcher cursor by bytes sent
                self->batcher_.advance(static_cast<size_t>(res));
            }
            batch.clear();
        }
    }

} // namespace aegis::net