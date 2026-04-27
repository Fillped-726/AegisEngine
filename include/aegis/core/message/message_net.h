#pragma once
#include <memory>
#include <string>
#include "aegis/core/message/message_base.h"
#include "aegis/net/packetPool.h"
#include "aegis/common/objectPool.h"
#include "aegis/net/connection.h" // 顶号需要用到

namespace aegis::core
{
    // 会话关闭消息
    struct SessionClosedMsg : public BasicMessage<SessionClosedMsg, MSG_TYPE_SESSION_CLOSED>
    {
        int session_id;
        explicit SessionClosedMsg(int sid) : session_id(sid) {}
    };

    // 业务转发
    struct ForwardPacketMsg : public BasicMessage<ForwardPacketMsg, MSG_TYPE_FORWARD_PACKET>
    {
        uint32_t msg_id;
        uint32_t seq_id;
        std::shared_ptr<std::string> shared_buf;

        ForwardPacketMsg(uint32_t id, std::shared_ptr<std::string> buf)
            : msg_id(id), seq_id(0), shared_buf(std::move(buf)) {}

        ForwardPacketMsg(uint32_t id, uint32_t seq, std::shared_ptr<std::string> buf)
            : msg_id(id), seq_id(seq), shared_buf(std::move(buf)) {}
    };

    // [新增] 重新绑定网络连接消息 (顶号使用)
    struct RebindConnectionMsg : public BasicMessage<RebindConnectionMsg, MSG_TYPE_REBIND_CONNECTION>
    {
        std::shared_ptr<net::Connection> new_conn;
    };

    // 网络消息 (带 ObjectPool 优化)
    class NetworkMessage;
    using NetworkMessagePool = aegis::core::ObjectPool<NetworkMessage, 100000>;

    struct NetworkMessage : public ActorMessage
    {
        aegis::net::PooledPacket pkt;
        int session_id = 0;

        NetworkMessage(aegis::net::PooledPacket &&p, int sid)
        {
            type_id = MSG_TYPE_NETWORK;
            pkt = std::move(p);
            session_id = sid;
        }

        void finalize() { NetworkMessagePool::instance().release(this); }

        void reset(aegis::net::PooledPacket &&p, int sid)
        {
            next.store(nullptr, std::memory_order_relaxed);
            type_id = MSG_TYPE_NETWORK;
            pkt = std::move(p);
            session_id = sid;
        }
    };
}