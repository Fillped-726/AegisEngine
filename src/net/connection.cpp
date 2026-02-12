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
            auto packet = std::make_unique<Packet>();
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

        {
            std::lock_guard<aegis::common::SpinLock> lock(outbox_.lock);
            outbox_.buffer.push_back(std::move(packet));
        }

        // 【核心变化】自注册 + 唤醒
        // 检查我是否已经在 Env 的脏名单里了？
        bool expected = false;
        if (in_pending_queue_.compare_exchange_strong(expected, true))
        {
            // 1. 把自己加入全局脏名单
            // (假设 Env 加了 pending_conns_ 成员)
            core::Env::instance().add_pending_connection(this);

            // 2. 按门铃唤醒 Env (如果它在睡)
            core::Env::instance().wake_up();
        }
    }

    void Connection::flush()
    {
        // 1. 重置标志
        in_pending_queue_.store(false, std::memory_order_relaxed);

        bool expected = false;
        if (!is_flushing_.compare_exchange_strong(expected, true))
        {
            return; // 已经有人在干活了，撤退
        }

        // 4. 【核心】发射协程！
        // 这里的 batch 会被 move 进协程帧里，自动保活！
        send_batch_coro(shared_from_this());

        // flush 函数结束，协程在后台挂起，等待 io_uring 完成
    }

    core::DetachedTask Connection::send_batch_coro(std::shared_ptr<Connection> self)
    {
        std::vector<PooledPacket> batch;

        while (true)
        {
            {
                std::lock_guard<aegis::common::SpinLock> lock(self->outbox_.lock);
                if (self->outbox_.buffer.empty())
                {
                    // 没有数据了，释放 flush 锁，结束协程
                    self->is_flushing_.store(false, std::memory_order_release);

                    // Double Check: 释放锁的瞬间可能又来了数据
                    // 如果刚释放就有新数据，且 Worker 没来得及触发 flush，我们需要回头
                    // 但由于 Worker 会 set in_pending_queue，Env 下一轮会再次调 flush
                    // 所以这里直接退出是安全的。
                    co_return;
                }
                batch.swap(self->outbox_.buffer);
            }
            // 1. 准备 iovecs
            size_t count = batcher_.prepare_batch(batch);

            while (!self->batcher_.is_empty())
            {
                auto iovs = self->batcher_.remaining_iovecs();
                int res = -1;
                try
                {
                    // co_await returns actual bytes sent
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
                    self->is_flushing_.store(false, std::memory_order_release);
                    co_return;
                }

                // [Fix] Advance the batcher cursor by bytes sent
                self->batcher_.advance(static_cast<size_t>(res));
            }
            batch.clear();
        }
    }

} // namespace aegis::net