#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>     // [Fix] 必须包含，否则 memcpy 报错
#include <arpa/inet.h> // for ntohl, htonl
#include <stdexcept>

namespace aegis::net
{

    // 协议常量定义
    // 1. 网络包长度头 (4字节): 也就是 Connection 第一次读取的长度
    constexpr size_t kPacketLenHeader = 4;
    // 2. 业务消息头 (4字节): 也就是 MsgID
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
            // Protobuf 允许解析空 Body (len=0)，只要 ptr 有效即可
            // 但如果 ptr 是 nullptr (包太短)，则返回 false
            if (!ptr && len > 0)
                return false;

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
            pkt.payload_.resize(kPacketMsgHeader + body_size);

            // 3. 写入 MsgID (Host -> Network)
            uint32_t net_id = htonl(msg_id);
            std::memcpy(pkt.payload_.data(), &net_id, kPacketMsgHeader);

            // 4. 写入 Protobuf 数据
            // SerializeToArray 直接写到 vector 的偏移位置
            msg.SerializeToArray(pkt.payload_.data() + kPacketMsgHeader, static_cast<int>(body_size));

            return pkt;
        }
    };

} // namespace aegis::net