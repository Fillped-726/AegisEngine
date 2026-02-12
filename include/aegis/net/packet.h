#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <bit>       // C++20 standard endianness
#include <algorithm> // for std::copy
#include <array>
#include <cassert>

// 你的日志库
#include "aegis/common/aegisLog.h"

// [C++20 Concepts] 约束：必须是 Protobuf 消息类型
namespace google::protobuf
{
    class Message;
}
template <typename T>
concept ProtobufMessage = std::is_base_of_v<google::protobuf::Message, T>;

namespace aegis::net
{
    // 协议常量
    constexpr size_t kPacketMsgHeader = 4;

    // [SSP 核心优化] SBO 阈值
    // 经验值：大部分游戏逻辑包（心跳、移动、状态）都在 256-512 字节以内
    // 设置为 1024 可以覆盖 99% 的场景，实现零堆分配
    constexpr size_t kSmallBufferSize = 1024;

    static constexpr size_t kMaxRetainSize = 64 * 1024;

    class Packet
    {
    public:
        Packet() = default;

        // 必须显式处理拷贝和移动，因为我们管理了原始内存指针
        Packet(const Packet &other);
        Packet &operator=(const Packet &other);

        // [Move Semantics] 移动构造是性能关键
        Packet(Packet &&other) noexcept;
        Packet &operator=(Packet &&other) noexcept;

        ~Packet();

        // --- 核心数据访问接口 ---

        // 获取消息 ID (C++20 Endian handling)
        [[nodiscard]] uint32_t msg_id() const;

        [[nodiscard]] const char *data() const;
        [[nodiscard]] size_t size() const;

        // Connection 读数据写入时调用
        // 关键：这里决定是用栈内存还是堆内存
        void alloc(size_t req_size);

        char *mutable_data();

        // 解析 Protobuf
        // 注意：模板函数必须定义在头文件中
        template <ProtobufMessage T>
        bool parse(T &msg) const
        {
            if (size_ <= kPacketMsgHeader)
                return false;

            // 跳过头部的 MsgID
            const void *body_ptr = data_ + kPacketMsgHeader;
            int body_len = static_cast<int>(size_ - kPacketMsgHeader);

            if (body_len == 0)
                return true;

            return msg.ParseFromArray(body_ptr, body_len);
        }

        // --- 发送侧工厂 ---

        // 注意：模板函数必须定义在头文件中
        template <ProtobufMessage T>
        void pack_into(uint32_t msg_id, const T &msg)
        {
            size_t body_size = msg.ByteSizeLong();
            size_t total_size = kPacketMsgHeader + body_size;

            alloc(total_size);

            // 1. 写入 MsgID (Big Endian)
            uint32_t net_id = (std::endian::native == std::endian::big)
                                  ? msg_id
                                  : __builtin_bswap32(msg_id);
            std::memcpy(data_, &net_id, kPacketMsgHeader);

            // 2. 写入 Body
            if (body_size > 0)
            {
                // 直接序列化到我们的 buffer 中
                msg.SerializeToArray(data_ + kPacketMsgHeader, static_cast<int>(body_size));
            }
        }

    private:
        // --- SBO 内存管理核心 ---

        // 栈上缓冲区 (Hot Memory)
        alignas(std::max_align_t) char stack_buf_[kSmallBufferSize];

        // 如果数据很大，使用堆内存
        char *heap_buf_ = nullptr;

        // 当前数据指针：指向 stack_buf_ 或 heap_buf_
        char *data_ = stack_buf_;

        // 当前逻辑大小
        size_t size_ = 0;

        // 当前容量
        size_t capacity_ = kSmallBufferSize;

        // 内部辅助函数声明
        void reset();
        void grow(size_t new_cap);
        void free_heap();
        void copy_from(const Packet &other);
        void move_from(Packet &&other);
    };

} // namespace aegis::net