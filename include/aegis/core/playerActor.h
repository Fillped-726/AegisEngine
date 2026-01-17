#pragma once
#include <atomic>
#include "aegis/core/actor.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/task.h"
#include "aegis/net/dispatcher.h"
#include "common.pb.h"
#include "scene.pb.h"

namespace aegis::protocol
{
    class PBPlayerInfo; // 前置声明
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
        // AOI / Scene Interface
        // ========================================================================

        [[nodiscard]] uint64_t GetID() const { return playerId_; }

        // [Thread-Safety Warning] 这些 Getter 可能会被 SceneActor 线程调用
        // 在 x64 上读取对齐的 float 通常是原子的，但在严格内存模型下存在风险
        [[nodiscard]] float GetX() const { return x_.load(std::memory_order_relaxed); }
        [[nodiscard]] float GetY() const { return y_.load(std::memory_order_relaxed); }

        // [New] Setter 使用原子操作，稍微安全一点
        void SetPos(float x, float y)
        {
            x_.store(x, std::memory_order_relaxed);
            y_.store(y, std::memory_order_relaxed);
        }

        // [New] 核心契约：将自己的外观数据写入 Proto
        // 注意：SceneActor 线程调用此函数时，传入的 x/y 应该是 Scene 自己维护的快照
        // 如果传入 -1 (默认)，则使用 PlayerActor 当前的原子坐标
        void WriteToProto(aegis::protocol::PBPlayerInfo *out_proto, float snapshotX = -999.0f, float snapshotY = -999.0f) const
        {
            if (!out_proto)
                return;

            out_proto->set_entity_id(playerId_);

            auto *pos = out_proto->mutable_pos();
            // 如果 Scene 传了坐标，就用 Scene 的（防止这一帧画面撕裂）
            // 否则读自己的原子坐标
            if (snapshotX > -900.0f)
            {
                pos->set_x(snapshotX);
                pos->set_y(snapshotY);
            }
            else
            {
                pos->set_x(GetX());
                pos->set_y(GetY());
            }
            pos->set_z(0.0f);

            // 静态数据或低频变动数据（Name, Skin），并发读取风险较低
            // 生产环境中这些字符串应该用 std::string_view 或加锁
            out_proto->set_name("Player_" + std::to_string(playerId_));
            out_proto->set_hp(100);
            out_proto->set_skin_id(1);
        }

        // [Public] 发送 Protobuf 消息
        template <typename T>
        void send_packet(uint32_t msg_id, const T &msg)
        {
            if (conn_)
            {
                auto pkt = net::Packet::pack(msg_id, msg);
                conn_->send(std::move(pkt));
            }
        }

        // [Public] 发送预序列化 Buffer (高性能广播专用)
        // 必须是 public，因为 SceneActor 需要调用它
        void send_buffer(uint32_t msg_id, const std::string &serialized_data)
        {
            if (!conn_)
                return;

            auto pkt = net::PacketPool::instance().acquire();
            size_t body_size = serialized_data.size();
            pkt->alloc(net::kPacketMsgHeader + body_size);

            // 写头 (Big Endian)
            uint32_t net_id = htonl(msg_id);
            std::memcpy(pkt->mutable_data(), &net_id, net::kPacketMsgHeader);

            // 写体 (Zero Copy logic handled by packet pool, but here we copy from string)
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
            if (msg->type_id == MSG_TYPE_NETWORK)
            {
                auto *net_msg = static_cast<core::NetworkMessage *>(msg);
                if (net_msg->pkt)
                {
                    // 启动协程处理业务逻辑
                    launch_task(net::Dispatcher::instance().dispatch(this, *net_msg->pkt));
                }
            }
            else if (msg->type_id == MSG_TYPE_SESSION_CLOSED)
            {
                auto *closed_msg = static_cast<core::SessionClosedMsg *>(msg);
                on_session_closed(closed_msg->session_id);
            }
        }

    private:
        // 辅助：启动并分离协程
        void launch_task(core::Task<void> task)
        {
            // C++20 立即执行 lambda
            [](core::Task<void> t) -> core::DetachedTask
            {
                co_await t;
            }(std::move(task));
        }

        void on_session_closed(int reason)
        {
            aegis::Log::instance().info("[PlayerActor] Session Closed | ID: {} | Reason: {}", playerId_, reason);
            // 这里应该通知 SceneActor 移除玩家： send(SceneLeaveMsg)
            conn_.reset();
        }

    private:
        std::shared_ptr<net::Connection> conn_;
        int fd_ = -1;

        uint64_t playerId_ = 0;

        // [Safety] 使用 atomic 避免最基本的读写撕裂，虽然不能完全解决多字段一致性
        std::atomic<float> x_{0.0f};
        std::atomic<float> y_{0.0f};
    };

} // namespace aegis::core