#pragma once
#include <vector>
#include <memory>
#include <cstring> // memcpy, memmove
#include <cstdint>
#include <algorithm> // std::min
#include "aegis/core/task.h"
#include "aegis/net/socket.h"
#include "aegis/net/packet.h"

namespace aegis::net
{

    class Connection
    {
        static constexpr size_t STASH_SIZE = 4096;
        Socket socket_;
        std::vector<char> stash_; // 使用 char 方便与 socket 接口交互

    public:
        // 移动构造 (Socket 只能移动)
        explicit Connection(Socket &&s) : socket_(std::move(s))
        {
            stash_.reserve(STASH_SIZE);
        }

        Connection(Connection &&) = default;
        Connection &operator=(Connection &&) = default;

        // 获取原始 Socket (用于获取 IP 等信息)
        const Socket &socket() const { return socket_; }

        // 【核心协程】读取一个完整包
        core::Task<std::unique_ptr<Packet>> read_packet()
        {

            // --- 阶段 1: 读取 Length (4 bytes) ---
            while (stash_.size() < 4)
            {
                size_t current_size = stash_.size();
                size_t needed = 4 - current_size;

                // 安全操作：先 resize 扩充有效空间
                stash_.resize(4);

                // 接收数据覆盖新分配的空间
                int n = co_await socket_.recv(stash_.data() + current_size, needed);

                if (n <= 0)
                    co_return nullptr;

                // 如果没读满，需要 resize 回去，否则下次 loop current_size 就错了
                if (n < (int)needed)
                {
                    stash_.resize(current_size + n);
                }
            }

            // 解码 Length (Big Endian)
            uint32_t total_len = 0;
            std::memcpy(&total_len, stash_.data(), 4);
            total_len = ntohl(total_len); // 网络序转主机序

            // 你的建议：MsgID(4) + Body
            // total_len 代表后续数据的总长度

            auto packet = std::make_unique<Packet>();
            // 预分配内存：MsgID(4) + Body
            packet->alloc(total_len);

            // --- 阶段 2: 处理 Stash 中的粘包数据 ---
            size_t stash_remaining = stash_.size() - 4;

            size_t copy_len = std::min((size_t)total_len, stash_remaining);

            if (copy_len > 0)
            {
                std::memcpy(packet->mutable_data(), stash_.data() + 4, copy_len);
            }

            // 清理 Stash
            size_t used_in_stash = 4 + copy_len;
            if (used_in_stash < stash_.size())
            {
                size_t left = stash_.size() - used_in_stash;
                std::memmove(stash_.data(), stash_.data() + used_in_stash, left);
                stash_.resize(left);
            }
            else
            {
                stash_.clear();
            }

            // --- 阶段 3: 读取剩余数据 (Zero-Copy) ---
            size_t filled = copy_len;
            while (filled < total_len)
            {
                size_t needed = total_len - filled;

                // 直接写入 packet payload，此时 payload 已经 resize 过了，是安全的
                int n = co_await socket_.recv(packet->mutable_data() + filled, needed);

                if (n <= 0)
                    co_return nullptr;
                filled += n;
            }

            co_return packet;
        }

        core::Task<void> send_packet(const Packet &packet)
        {
            // 1. 准备数据
            // 为了保证 io_uring 异步期间数据的有效性，以及减少系统调用次数，
            // 建议将 header 和 body 拼接到一个临时 vector 中发送
            // (虽然分两次 send 也能用，但为了性能和安全，这里演示拼包)

            std::vector<char> buffer;
            uint32_t body_size = packet.payload_.size();
            uint32_t net_len = htonl(body_size);

            buffer.resize(4 + body_size);

            // 拷贝 Header
            std::memcpy(buffer.data(), &net_len, 4);

            // 拷贝 Body
            if (body_size > 0)
            {
                std::memcpy(buffer.data() + 4, packet.payload_.data(), body_size);
            }

            // 2. 发送
            // 注意：buffer 是局部变量，但在协程挂起期间它保存在协程帧(Heap)中，
            // 只要我们在 send 完成前不销毁协程帧(co_await 保证了这点)，就是安全的。
            co_await socket_.send(buffer.data(), buffer.size());

            co_return;
        }
    };

} // namespace aegis::net