#pragma once
#include "aegis/core/actor.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/task.h"
#include "aegis/net/dispatcher.h"
#include "common.pb.h"
#include "scene.pb.h"

namespace aegis::protocol
{
    class PBPlayerInfo;
}

namespace aegis::core
{
    class PlayerActor : public Actor
    {
    public:
        explicit PlayerActor(std::shared_ptr<net::Connection> conn, uint64_t playerId = 0)
            : conn_(std::move(conn)), fd_(conn_->fd()), playerId_(playerId)
        {
            aegis::Log::instance().debug("PlayerActor Created | ID: {} | FD: {}", playerId_, fd_);
        }

        ~PlayerActor()
        {
            aegis::Log::instance().debug("PlayerActor Destroyed | ID: {}", playerId_);
        }

        // ========================================================================
        // AOI / Scene Interface (新增的场景契约)
        // ========================================================================

        // [New] Getter for AOI
        [[nodiscard]] uint64_t GetID() const { return playerId_; }
        [[nodiscard]] float GetX() const { return x_; }
        [[nodiscard]] float GetY() const { return y_; }

        // [New] Setter for Movement
        void SetPos(float x, float y)
        {
            x_ = x;
            y_ = y;
        }

        // [New] 核心契约：将自己的外观数据写入 Proto
        // 这允许 Scene 在广播 SCEnterViewNtf 时获取你的信息
        void WriteToProto(aegis::protocol::PBPlayerInfo *out_proto) const
        {
            if (!out_proto)
                return;
            out_proto->set_entity_id(playerId_);

            auto *pos = out_proto->mutable_pos();
            pos->set_x(x_);
            pos->set_y(y_);
            pos->set_z(0.0f); // 2D AOI 忽略 Z

            // Mock 一些外观数据，实际项目中这里读取 DB 或 Config
            out_proto->set_name("Player_" + std::to_string(playerId_));
            out_proto->set_hp(100);
            out_proto->set_skin_id(1);
        }

        // [新增 Public 接口] 供 Dispatcher 回调使用
        template <typename T>
        void send_packet(uint32_t msg_id, const T &msg)
        {
            if (conn_)
            {
                // 使用 Packet::pack 工厂函数打包
                auto pkt = net::Packet::pack(msg_id, msg);
                conn_->send(std::move(pkt));
            }
        }
        // 2. [New] 发送原始 Buffer (高性能路径 - 序列化复用)
        // 用于 Scene 广播时，直接把已经序列化好的 string 发送出去
        void send_buffer(uint32_t msg_id, const std::string &serialized_data)
        {
            if (!conn_)
                return;

            // 从池中申请包
            auto pkt = net::PacketPool::instance().acquire();

            // 计算总大小
            size_t body_size = serialized_data.size();
            pkt->alloc(net::kPacketMsgHeader + body_size);

            // 写头 (Host to Network Long)
            uint32_t net_id = htonl(msg_id);
            std::memcpy(pkt->mutable_data(), &net_id, net::kPacketMsgHeader);

            // 写体 (直接内存拷贝，无需再次 Protobuf Serialize)
            if (body_size > 0)
            {
                std::memcpy(pkt->mutable_data() + net::kPacketMsgHeader, serialized_data.data(), body_size);
            }

            conn_->send(std::move(*pkt));
        }

    protected:
        // --- Worker 线程执行此函数 ---
        void handle_message(core::ActorMessage *msg) override
        {
            // 1. 网络消息处理
            if (msg->type_id == 1)
            {
                auto *net_msg = static_cast<core::NetworkMessage *>(msg);

                // [Fix] 使用 Dispatcher 分发
                // 注意：net_msg->pkt 是 PooledPacket (unique_ptr)，dispatch 需要 const Packet&
                // 所以我们需要解引用: *net_msg->pkt
                if (net_msg->pkt)
                {
                    launch_task(net::Dispatcher::instance().dispatch(shared_from_this(), *net_msg->pkt));
                }
            }
            // 2. 系统消息：连接断开
            else if (msg->type_id == 0)
            {
                auto *closed_msg = static_cast<core::SessionClosedMsg *>(msg);
                on_session_closed(closed_msg->session_id);
            }
        }

    private:
        // [Fix] 协程启动辅助函数
        // 将惰性的 Task<void> 转换为 DetachedTask 并立即执行
        void launch_task(core::Task<void> task)
        {
            [](core::Task<void> t) -> core::DetachedTask
            {
                co_await t;
            }(std::move(task));
        }

        void on_session_closed(int reason)
        {
            aegis::Log::instance().info("[Actor] Session Closed | ID: {} | Reason: {}", playerId_, reason);
            // [TODO] Persistence logic here
            conn_.reset();
        }

    private:
        std::shared_ptr<net::Connection> conn_;
        int fd_ = -1;
        // [New] 场景属性
        uint64_t playerId_ = 0;
        float x_ = 0.0f;
        float y_ = 0.0f;
    };

} // namespace aegis::gate