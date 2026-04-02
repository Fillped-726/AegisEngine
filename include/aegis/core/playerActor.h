#pragma once
#include <atomic>
#include "aegis/common/spinLock.h"
#include "aegis/core/actor_traits.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/task.h"
#include "aegis/net/dispatcher.h"
#include "aegis/common/scopeGuard.h"
#include "cs_battle.pb.h"
#include "common.pb.h"

namespace aegis::common
{
    class PBPlayerInfo; // 前置声明
}

namespace aegis::core
{
    class PlayerActor : public PooledActor<PlayerActor>
    {
    public:
        enum DirtyFlag : uint8_t
        {
            DIRTY_NONE = 0,
            DIRTY_POS = 1 << 0,
            DIRTY_HP = 1 << 1,
            // ... 可扩展其他属性
        };

        void MarkDirty(uint8_t flag) { dirty_mask_ |= flag; }
        void ClearDirty() { dirty_mask_ = DIRTY_NONE; }
        bool IsDirty(uint8_t flag) const { return (dirty_mask_ & flag) != 0; }
        bool HasAnyDirty() const { return dirty_mask_ != DIRTY_NONE; }

        PlayerActor(ActorID id, std::shared_ptr<net::Connection> conn)
            : PooledActor()
        {
            reset(id, conn);
        }

        virtual ~PlayerActor()
        {
            aegis::Log::instance().debug("PlayerActor Destroyed | ID: {}", playerId_);
        }

        void reset(ActorID id, std::shared_ptr<net::Connection> conn)
        {
            // 1. 重置基类 (生成新 ID，设置新父亲)
            base_reset(id, ActorID(0));

            // 2. 交换/重置连接
            // 使用 std::move 减少引用计数操作
            // 旧的 conn_ 会在这里析构（引用计数-1），如果计数归零则关闭连接
            conn_ = std::move(conn);

            // 3. 同步 FD
            fd_ = conn_ ? conn_->fd() : -1;

            // 4. 重置逻辑数据
            playerId_ = 0;

            // 5. 重置坐标 (Atomic)
            // 使用 memory_order_relaxed 即可，因为此时 Actor 还没对其他线程可见
            x_.store(0.0f, std::memory_order_relaxed);
            y_.store(0.0f, std::memory_order_relaxed);
        }

        // ========================================================================
        // AOI / Scene Interface
        // ========================================================================

        [[nodiscard]] uint64_t GetID() const { return playerId_; }

        // [Thread-Safety Warning] 这些 Getter 可能会被 SceneActor 线程调用
        // 在 x64 上读取对齐的 float 通常是原子的，但在严格内存模型下存在风险
        [[nodiscard]] float GetX() const { return x_.load(std::memory_order_relaxed); }
        [[nodiscard]] float GetY() const { return y_.load(std::memory_order_relaxed); }
        float GetZ() const { return z_; }

        // [New] Setter 使用原子操作，稍微安全一点
        void SetPos(float x, float y, float z = 0.0f, uint8_t dirty_flag = DIRTY_POS)
        {
            x_.store(x, std::memory_order_relaxed);
            y_.store(y, std::memory_order_relaxed);
            z_ = z;
            MarkDirty(dirty_flag);
        }

        // [New] 核心契约：将自己的外观数据写入 Proto
        // 注意：SceneActor 线程调用此函数时，传入的 x/y 应该是 Scene 自己维护的快照
        // 如果传入 -1 (默认)，则使用 PlayerActor 当前的原子坐标
        void WriteToProto(aegis::common::PBPlayerInfo *out_proto, float snapshotX = -999.0f, float snapshotY = -999.0f) const
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

                auto pkt = net::PacketPool::instance().acquire();

                pkt->pack_into(msg_id, msg);

                // 4. 发送
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

            conn_->send(std::move(pkt));
        }

        void set_player_id(uint64_t pid) { playerId_ = pid; }
        [[nodiscard]] uint64_t get_player_id() const { return playerId_; }

        uint32_t get_aoi_grid_index() const { return aoi_grid_index; }
        void set_aoi_grid_index(uint32_t index) { aoi_grid_index = index; }

    protected:
        void handle_message(core::ActorMessage *msg)
        {
            if (!msg)
                return;

            // 仅在 Trace 级别打印，减少性能损耗
            if (msg->type_id != 0)
            {
                Log::instance().debug("[Trace] PlayerActor Recv Msg. TypeID: {}", (int)msg->type_id);
            }

            switch (msg->type_id)
            {
            case MSG_TYPE_NETWORK:
            {
                auto *net_msg = static_cast<core::NetworkMessage *>(msg);
                if (net_msg->pkt)
                {
                    // 启动协程处理业务逻辑
                    launch_task(net::Dispatcher::instance().dispatch(this, *net_msg->pkt));
                }
                break;
            }

            case MSG_TYPE_SESSION_CLOSED:
            {
                auto *closed_msg = static_cast<core::SessionClosedMsg *>(msg);
                // 维持原有锁逻辑，保护 session 状态
                std::lock_guard<common::SpinLock> guard(lock_);
                on_session_closed(closed_msg->session_id);
                break;
            }

            // =========================================================
            // 【新增】处理由 SceneActor 异步投递过来的转发请求
            // =========================================================
            case MSG_TYPE_FORWARD_PACKET:
            {
                auto *fwd = static_cast<ForwardPacketMsg *>(msg);

                // 直接调用你已有的 Public 接口 send_buffer
                // 该接口内部会处理 PacketPool 申请、大端序转换及实际发送
                if (fwd->shared_buf)
                {
                    this->send_buffer(fwd->msg_id, *(fwd->shared_buf));
                }
                break;
            }

            default:
                break;
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
            conn_.reset();
        }

    private:
        std::shared_ptr<net::Connection> conn_;
        int fd_ = -1;

        uint64_t playerId_ = 0;
        uint8_t dirty_mask_ = DIRTY_NONE;

        uint32_t aoi_grid_index = -1;

        common::SpinLock lock_;

        // [Safety] 使用 atomic 避免最基本的读写撕裂，虽然不能完全解决多字段一致性
        std::atomic<float> x_{0.0f};
        std::atomic<float> y_{0.0f};
        float z_{0.0f};
    };

} // namespace aegis::core