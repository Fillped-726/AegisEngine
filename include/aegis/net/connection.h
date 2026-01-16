#pragma once
#include <vector>
#include <deque>
#include <memory>
#include <cstring>
#include <arpa/inet.h>
#include "aegis/core/task.h"
#include "aegis/net/socket.h"
#include "aegis/net/packet.h"
#include "aegis/common/spinLock.h"
#include "aegis/net/packetPool.h"

namespace aegis::net
{
    // [Fix] 继承 enable_shared_from_this 以支持异步保活
    class Connection : public std::enable_shared_from_this<Connection>
    {
        static constexpr size_t STASH_SIZE = 4096;
        Socket socket_;

        // 读取缓冲区
        std::vector<char> rx_buffer_;
        size_t rx_len_ = 0;

        // 发送队列 (Outbox)
        struct Outbox
        {
            std::deque<Packet> queue;
            bool sending = false;
            aegis::common::SpinLock lock;
        } outbox_;

    public:
        explicit Connection(Socket &&s) : socket_(std::move(s))
        {
            rx_buffer_.resize(STASH_SIZE);
        }

        Connection(Connection &&) = delete; // 继承 shared_from_this 后通常禁止移动，防止指针错乱
        Connection &operator=(Connection &&) = delete;
        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;

        const Socket &socket() const { return socket_; }
        int fd() const { return socket_.native_handle(); }

        // --- 读取逻辑 ---
        core::Task<PooledPacket> read_packet()
        {
            auto packet = PacketPool::instance().acquire();
            // [A] Ensure Header
            while (rx_len_ < 4)
            {
                if (rx_len_ == rx_buffer_.size())
                {
                    rx_buffer_.resize(rx_buffer_.size() * 2);
                }
                int n = co_await socket_.recv(rx_buffer_.data() + rx_len_, rx_buffer_.size() - rx_len_);
                if (n <= 0)
                    co_return nullptr;
                rx_len_ += n;
            }

            // [B] Parse Length
            uint32_t net_len;
            std::memcpy(&net_len, rx_buffer_.data(), 4);
            uint32_t body_len = ntohl(net_len);

            uint32_t total_packet_len = 4 + body_len;

            // Security Check
            if (total_packet_len > 10 * 1024 * 1024)
                co_return nullptr;

            // [C] Ensure Buffer
            if (rx_buffer_.size() < total_packet_len)
            {
                rx_buffer_.resize(total_packet_len);
            }

            // [D] Read Full Body
            while (rx_len_ < total_packet_len)
            {
                int n = co_await socket_.recv(rx_buffer_.data() + rx_len_, rx_buffer_.size() - rx_len_);
                if (n <= 0)
                    co_return nullptr;
                rx_len_ += n;
            }

            // [E] Extract Packet
            packet->alloc(body_len);

            if (body_len > 0)
            {
                std::memcpy(packet->mutable_data(), rx_buffer_.data() + 4, body_len);
            }

            // [F] Stash Handling
            size_t remaining = rx_len_ - total_packet_len;
            if (remaining > 0)
            {
                std::memmove(rx_buffer_.data(), rx_buffer_.data() + total_packet_len, remaining);
            }
            rx_len_ = remaining;

            co_return packet;
        }

        // --- 发送逻辑 ---
        virtual void send(Packet packet)
        {
            size_t payload_size = packet.payload_.size();
            bool need_start_loop = false;

            {
                std::lock_guard<aegis::common::SpinLock> lock(outbox_.lock);
                outbox_.queue.push_back(std::move(packet));

                if (!outbox_.sending)
                {
                    outbox_.sending = true;
                    need_start_loop = true;
                    aegis::Log::instance().debug("[Connection] Spawning send_loop. Size: {}", payload_size);
                }
            }

            if (need_start_loop)
            {
                // [Critical Fix] 传递 shared_from_this() 确保协程运行期间 Connection 不会析构
                send_loop(shared_from_this());
            }
        }

    private:
        // [Critical Fix] 参数 self 保持引用计数 +1
        core::DetachedTask send_loop(std::shared_ptr<Connection> /*self*/)
        {
            // aegis::Log::instance().debug("[Connection] Send loop active.");

            while (true)
            {
                Packet pkt;
                {
                    std::lock_guard<aegis::common::SpinLock> lock(outbox_.lock);
                    if (outbox_.queue.empty())
                    {
                        outbox_.sending = false;
                        break; // Loop exits, 'self' destructs, refcount -1
                    }
                    pkt = std::move(outbox_.queue.front());
                    outbox_.queue.pop_front();
                }

                try
                {
                    co_await internal_send(pkt);
                }
                catch (const std::exception &e)
                {
                    aegis::Log::instance().error("[Connection] Send Error: {}", e.what());
                    break; // Error, exit loop
                }
                catch (...)
                {
                    break;
                }
            }
        }

        core::Task<void> internal_send(const Packet &pkt)
        {
            uint32_t body_size = pkt.payload_.size();
            uint32_t net_len = htonl(body_size);
            size_t total_size = 4 + body_size;

            if (tx_buffer_.size() < total_size)
            {
                tx_buffer_.resize(total_size);
            }

            std::memcpy(tx_buffer_.data(), &net_len, 4);
            std::memcpy(tx_buffer_.data() + 4, pkt.payload_.data(), body_size);

            co_await socket_.send(tx_buffer_.data(), total_size);
        }

        std::vector<char> tx_buffer_;
    };
}