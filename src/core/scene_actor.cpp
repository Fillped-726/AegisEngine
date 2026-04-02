#include "aegis/core/scene_actor.h"
#include "aegis/common/aegisLog.h"
#include "cs_battle.pb.h"
#include "ids.pb.h"
#include "aegis/common/actor_utils.h"
#include "aegis/net/packet_builder.h"

namespace aegis::core
{
    using namespace aegis::cs::battle;
    using namespace aegis::common;

    SceneActor::SceneActor(ActorID self_id, float width, float height, float cellSize)
        : PooledActor(),
          aoi_(width, height, cellSize)
    {

        base_reset(self_id, ActorID(0));

        // 预分配内存，避免后续动态扩容
        cachedEnterIds_.reserve(50);
        cachedLeaveIds_.reserve(50);

        Log::instance().info("[SceneActor] Created & Initialized Grid: {}x{} (Cell: {}) ID: {}",
                             width, height, cellSize, self_id.raw);
    }

    void SceneActor::reset(ActorID self_id, float width, float height, float cellSize)
    {
        // 1. 重置 Actor 基类 (ID 和 父节点)
        base_reset(self_id, ActorID(0));

        // 2. 重置 AOI 网格
        // 这里会根据尺寸决定是否重分配内存，通常是零分配
        aoi_.reset(width, height, cellSize);

        // 3. 清理玩家映射表
        // clear() 只是设置 size=0，不会释放 bucket 内存，复用极快
        actors_.clear();

        // 4. 清理缓存容器
        cachedEnterIds_.clear();
        cachedLeaveIds_.clear();

        // Log::instance().debug("[SceneActor] Resetted for parent {}", parent_id);
    }

    void SceneActor::handle_message(ActorMessage *msg)
    {
        Log::instance().debug("[Trace] 3. SceneActor Recv Msg. TypeID: {}", (int)msg->type_id);
        switch (msg->type_id)
        {
        case MSG_TYPE_SCENE_ENTER:
            OnHandleEnter(static_cast<SceneEnterMsg *>(msg));
            break;
        case MSG_TYPE_SCENE_LEAVE:
            OnHandleLeave(static_cast<SceneLeaveMsg *>(msg));
            break;
        case MSG_TYPE_SCENE_MOVE:
            OnHandleMove(static_cast<SceneMoveMsg *>(msg));
            break;
        default:
            Log::instance().warn("[SceneActor] Unknown message type: {}", msg->type_id);
            break;
        }
    }

    void SceneActor::OnHandleEnter(SceneEnterMsg *msg)
    {
        assert(msg != nullptr && "SceneEnterMsg cannot be null");

        if (!is_ticking_)
        {
            is_ticking_ = true;
            schedule_timer(50, [this]()
                           { OnTick(); });
            Log::instance().info("[SceneActor] Scene woken up. Tick scheduling started.");
        }

        ActorID actor_id = msg->actor_id;
        auto *base_actor = ActorRegistry::instance().get(actor_id);
        if (!base_actor)
            return;

        auto *player = static_cast<PlayerActor *>(base_actor);
        actors_[actor_id.raw] = player;

        uint32_t grid_index = aoi_.Add(actor_id.raw, msg->x, msg->y);
        player->set_aoi_grid_index(grid_index);

        std::vector<uint64_t> neighbor_ids;
        aoi_.GetViewEntityIds(grid_index, neighbor_ids);

        if (neighbor_ids.empty())
            return;

        // 1. 构建发给邻居的包："我来了"
        auto shared_data_to_others = net::PacketBuilder::BuildEnterView({msg->player_id, msg->x, msg->y});

        // 2. 收集邻居信息，准备发给"我"
        std::vector<net::EntityViewInfo> neighbors_info;
        neighbors_info.reserve(neighbor_ids.size());

        for (uint64_t neighbor_raw_id : neighbor_ids)
        {
            if (neighbor_raw_id == actor_id.raw)
                continue;

            auto it = actors_.find(neighbor_raw_id);
            if (it != actors_.end())
            {
                PlayerActor *neighbor = it->second;
                neighbors_info.push_back({neighbor->get_player_id(), neighbor->GetX(), neighbor->GetY()});

                // 通知邻居
                auto *forward_msg = new ForwardPacketMsg(ids::SC_ENTER_VIEW, shared_data_to_others);
                dispatch_msg(neighbor, forward_msg);
            }
        }

        // 3. 通知"我"周围有谁
        if (!neighbors_info.empty())
        {
            auto shared_data_to_me = net::PacketBuilder::BuildEnterView(neighbors_info);
            auto *self_forward = new ForwardPacketMsg(ids::SC_ENTER_VIEW, shared_data_to_me);
            dispatch_msg(player, self_forward);
        }
    }

    void SceneActor::OnHandleLeave(SceneLeaveMsg *msg)
    {
        assert(msg != nullptr && "SceneLeaveMsg cannot be null");

        uint64_t raw_id = msg->actor_id.raw;
        auto it = actors_.find(raw_id);
        if (it == actors_.end())
            return;

        PlayerActor *player = it->second;
        uint32_t grid_index = player->get_aoi_grid_index();

        std::vector<uint64_t> neighbors;
        aoi_.GetViewEntityIds(grid_index, neighbors);

        aoi_.RemoveByGridIndex(raw_id, grid_index);

        if (!neighbors.empty())
        {
            // 委托 Builder 构建离开视野的广播包
            auto shared_buf = net::PacketBuilder::BuildLeaveView(msg->player_id);

            for (uint64_t neighbor_raw_id : neighbors)
            {
                if (neighbor_raw_id == raw_id)
                    continue;

                if (auto neighbor_it = actors_.find(neighbor_raw_id); neighbor_it != actors_.end())
                {
                    auto *forward_msg = new ForwardPacketMsg(ids::SC_LEAVE_VIEW, shared_buf);
                    dispatch_msg(neighbor_it->second, forward_msg);
                }
            }
        }

        actors_.erase(it);
    }

    // ==========================================
    // 2. 重写 OnHandleMove：只记状态，坚决不发包！
    // ==========================================
    void SceneActor::OnHandleMove(SceneMoveMsg *msg)
    {
        uint64_t mover_actor_id = msg->actor_id.raw;
        auto mover = GetPlayer(mover_actor_id);
        if (!mover)
            return;

        // 仅更新内存坐标，并打上脏标记
        mover->SetPos(msg->newX, msg->newY, 0.0f, msg->direction);

        // 扔进同步队列，等待 Tick 处理
        sync_mgr_.AddDirtyPlayer(mover);
    }

    // ==========================================
    // 3. 新增 OnTick 驱动函数 (在头文件中声明 void OnTick();)
    // ==========================================
    void SceneActor::OnTick()
    {
        // 调用同步管理器的 Tick，传入处理 Enter/Leave 和 发包的 Lambda 闭包
        sync_mgr_.Tick(aoi_,
                       // Callback 1: 处理视野跨格 (完美复用你原有的拆解逻辑)
                       [this](PlayerActor *mover, const std::vector<uint64_t> &enterIds, const std::vector<uint64_t> &leaveIds)
                       { this->ProcessAoiEnterLeave(mover, enterIds, leaveIds); },
                       // Callback 2: 批量发送 SCMoveNtfBatch
                       [this](uint64_t targetActorId, std::shared_ptr<std::string> sharedBuf)
                       {
                // IDs::SC_MOVE_NTF_BATCH 需要在你的 message_id 中定义
                this->SendSharedBuffer(targetActorId, ids::SC_MOVE_NTF, sharedBuf); });

        // 如果 schedule_timer 是一次性的，需要在这里重新注册下一次 Tick
        schedule_timer(50, [this]()
                       { OnTick(); });
    }

    // ==========================================
    // 4. 新增辅助函数 (把原先 OnHandleMove 里处理 Enter/Leave 的代码抽出来)
    // ==========================================
    void SceneActor::ProcessAoiEnterLeave(PlayerActor *mover,
                                          const std::vector<uint64_t> &enter_ids,
                                          const std::vector<uint64_t> &leave_ids)
    {
        if (!mover)
            return;

        uint64_t mover_uid = mover->get_player_id();

        // --- 处理跨网格新进入视野 ---
        if (!enter_ids.empty())
        {
            auto shared_data_to_others = net::PacketBuilder::BuildEnterView({mover_uid, mover->GetX(), mover->GetY()});

            std::vector<net::EntityViewInfo> new_neighbors_info;
            new_neighbors_info.reserve(enter_ids.size());

            for (uint64_t neighbor_id : enter_ids)
            {
                if (auto *neighbor = GetPlayer(neighbor_id))
                {
                    new_neighbors_info.push_back({neighbor->get_player_id(), neighbor->GetX(), neighbor->GetY()});
                    SendSharedBuffer(neighbor_id, ids::SC_ENTER_VIEW, shared_data_to_others);
                }
            }

            if (!new_neighbors_info.empty())
            {
                auto shared_data_to_me = net::PacketBuilder::BuildEnterView(new_neighbors_info);
                SendSharedBuffer(mover->id().raw, ids::SC_ENTER_VIEW, shared_data_to_me);
            }
        }

        // --- 处理跨网格离开视野 ---
        if (!leave_ids.empty())
        {
            auto shared_data_to_others = net::PacketBuilder::BuildLeaveView(mover_uid);

            std::vector<uint64_t> leave_neighbors_uids;
            leave_neighbors_uids.reserve(leave_ids.size());

            for (uint64_t neighbor_id : leave_ids)
            {
                if (auto *neighbor = GetPlayer(neighbor_id))
                {
                    leave_neighbors_uids.push_back(neighbor->get_player_id());
                    SendSharedBuffer(neighbor_id, ids::SC_LEAVE_VIEW, shared_data_to_others);
                }
            }

            if (!leave_neighbors_uids.empty())
            {
                auto shared_data_to_me = net::PacketBuilder::BuildLeaveView(leave_neighbors_uids);
                SendSharedBuffer(mover->id().raw, ids::SC_LEAVE_VIEW, shared_data_to_me);
            }
        }
    }

    PlayerActor *SceneActor::GetPlayer(uint64_t actorId) const
    {
        auto it = actors_.find(actorId);
        return (it != actors_.end()) ? it->second : nullptr;
    }

    template <typename T>
    void SceneActor::SendPacket(uint64_t targetId, uint32_t msgId, const T &proto)
    {
        if (auto actor = GetPlayer(targetId))
        {
            auto *forward = new ForwardPacketMsg(msgId, std::make_shared<std::string>(proto.SerializeAsString()));
            dispatch_msg(actor, forward);
        }
    }

    void SceneActor::SendSharedBuffer(uint64_t targetId, uint32_t msgId, std::shared_ptr<std::string> sharedBuf)
    {
        if (auto actor = GetPlayer(targetId))
        {
            // 多个 ForwardPacketMsg 共享同一个 buffer 内存
            auto *forward = new ForwardPacketMsg(msgId, sharedBuf);
            dispatch_msg(actor, forward);
        }
    }

} // namespace aegis::core