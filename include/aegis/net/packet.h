#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <cstring>     // for memcpy
#include <arpa/inet.h> // for ntohl, htonl
#include <stdexcept>

// 必须包含你的日志封装，而不是直接用 spdlog
#include "aegis/common/aegisLog.h"

namespace aegis::net
{

    // 协议常量定义
    // 1. 业务消息头 (4字节): 也就是 MsgID
    constexpr size_t kPacketMsgHeader = 4;

    class Packet
    {
    public:
        // 内存布局: [MsgID (4B)] + [Protobuf Body]
        // 注意: 这里不包含 Length 头，Length 头在 Connection 层已经被“吃”掉了
        std::vector<char> payload_;

        Packet() = default;

        // --- 接收侧接口 (Getter) ---

        // 获取消息 ID (自动处理大小端)
        uint32_t msg_id() const
        {
            if (payload_.size() < kPacketMsgHeader)
                return 0;

            uint32_t net_id;
            std::memcpy(&net_id, payload_.data(), 4);
            return ntohl(net_id);
        }

        // 获取纯 Body 的指针 (跳过 MsgID)
        const void *body_ptr() const
        {
            if (payload_.size() <= kPacketMsgHeader)
                return nullptr;
            return payload_.data() + kPacketMsgHeader;
        }

        // 获取纯 Body 的长度
        size_t body_len() const
        {
            if (payload_.size() <= kPacketMsgHeader)
                return 0;
            return payload_.size() - kPacketMsgHeader;
        }

        // 【高内聚接口】直接解析为 Protobuf 对象
        // 用法: if (pkt.parse(login_req)) { ... }
        template <typename T>
        bool parse(T &msg) const
        {
            const void *ptr = body_ptr();
            size_t len = body_len();

            // 调试日志 (建议仅在 LogLevel::Debug 下开启)
            /*
            if (len > 0 && ptr != nullptr)
            {
                const uint8_t *byte_ptr = static_cast<const uint8_t *>(ptr);
                aegis::Log::instance().debug("[Packet] Parse: Len={}, HeaderBytes=[{:02x} {:02x} {:02x} {:02x}]",
                                             len,
                                             byte_ptr[0],
                                             (len > 1 ? byte_ptr[1] : 0),
                                             (len > 2 ? byte_ptr[2] : 0),
                                             (len > 3 ? byte_ptr[3] : 0));
            }
            else
            {
                aegis::Log::instance().debug("[Packet] Parse: Len is 0 or ptr is null");
            }
            */

            // Protobuf 允许解析空 Body (len=0)，只要 ptr 有效即可
            // 但如果 payload 还没 MsgID 长 (ptr=nullptr)，则肯定失败
            if (!ptr && payload_.size() < kPacketMsgHeader)
                return false;

            // 特殊情况：有 MsgID 但 Body 为空 (len=0) -> 这是一个合法的空消息
            if (len == 0)
            {
                return true; // 或者是 msg.Clear() ? 视业务而定
            }

            // 这里的 int len 转换是安全的，因为限制了包大小
            return msg.ParseFromArray(ptr, static_cast<int>(len));
        }

        // --- Connection 侧接口 (Writer) ---

        // 预留空间 (Connection 读数据前调用)
        // size = MsgID(4) + ProtoBodyLen
        void alloc(size_t size)
        {
            payload_.resize(size);
        }

        // SSP 优化：重置对象状态，但保留内存 Capacity
        // 供 ObjectPool 调用
        void reset()
        {
            // 关键点：std::vector::clear() 不会释放 capacity()
            // 下次 resize 时只要不超过 capacity 就不需要 malloc
            payload_.clear();
        }

        char *mutable_data() { return payload_.data(); }

        // --- 发送侧接口 (Factory) ---

        // 【新增】打包工厂：将 MsgID 和 Protobuf 对象打包成 Packet
        // 用法: auto pkt = Packet::pack(MsgID::SC_LOGIN_RES, res);
        template <typename T>
        static Packet pack(uint32_t msg_id, const T &msg)
        {
            Packet pkt;
            // 1. 序列化 Protobuf
            size_t body_size = msg.ByteSizeLong();

            // 2. 分配总空间 (MsgID + Body)
            // vector resize 会进行 zero-initialization，略有开销但安全
            pkt.payload_.resize(kPacketMsgHeader + body_size);

            // 3. 写入 MsgID (Host -> Network)
            uint32_t net_id = htonl(msg_id);
            std::memcpy(pkt.payload_.data(), &net_id, kPacketMsgHeader);

            // 4. 写入 Protobuf 数据
            // SerializeToArray 直接写到 vector 的偏移位置
            if (body_size > 0)
            {
                msg.SerializeToArray(pkt.payload_.data() + kPacketMsgHeader, static_cast<int>(body_size));
            }

            return pkt;
        }
    };

} // namespace aegis::net