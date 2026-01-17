#include "aegis/core/scene_actor.h"
#include "aegis/common/aegisLog.h"
// 引入生成的 Protobuf 头文件
#include "scene.pb.h"

namespace aegis::core
{
    using namespace aegis::protocol;
    using namespace aegis::common;

    // [Config] 协议 ID 定义 (与客户端约定的常量)
    constexpr uint32_t SC_MOVE_NTF = 1002;
    constexpr uint32_t SC_ENTER_VIEW_NTF = 1003;
    constexpr uint32_t SC_LEAVE_VIEW_NTF = 1004;

    SceneActor::SceneActor(float width, float height, float cellSize)
        : aoi_(width, height, cellSize)
    {
        cachedEnterIds_.reserve(50);
        cachedLeaveIds_.reserve(50);
        Log::instance().info("[SceneActor] Initialized Grid: {}x{} (Cell: {})", width, height, cellSize);
    }

    void SceneActor::handle_message(ActorMessage *msg)
    {
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
        auto player = msg->player;
        if (!player)
            return;

        uint64_t id = player->GetID();
        float x = msg->x;
        float y = msg->y;

        actors_[id] = player;
        aoi_.Add(id, x, y);

        // 这里可以补充进入视野的广播逻辑 (类似 Move 的 EnterView 处理)
        // 简单起见，暂略，通常 Enter 也会触发一次 GetViewEntityIds 来同步周围玩家

        Log::instance().debug("[SceneActor] Player {} enter at ({}, {})", id, x, y);
    }

    void SceneActor::OnHandleLeave(SceneLeaveMsg *msg)
    {
        uint64_t id = msg->entityId;
        auto it = actors_.find(id);
        if (it != actors_.end())
        {
            auto player = it->second;
            aoi_.Remove(id, player->GetX(), player->GetY());
            actors_.erase(it);
            Log::instance().debug("[SceneActor] Player {} leave", id);
        }
    }

    void SceneActor::OnHandleMove(SceneMoveMsg *msg)
    {
        uint64_t moverId = msg->entityId;
        float oldX = msg->oldX;
        float oldY = msg->oldY;
        float newX = msg->newX;
        float newY = msg->newY;

        // 1. AOI 计算差分
        // cachedEnterIds_ 和 cachedLeaveIds_ 是成员变量，串行访问绝对安全
        bool moved = aoi_.Move(moverId, oldX, oldY, newX, newY, cachedEnterIds_, cachedLeaveIds_);

        if (!moved)
            return;

        auto mover = GetPlayer(moverId);
        if (!mover)
            return;

        // 2. 处理 [Enter View]
        if (!cachedEnterIds_.empty())
        {
            // A. 告诉 mover (我自己)："你看见了这些新邻居"
            SCEnterViewNtf ntfToSelf;
            for (uint64_t neighborId : cachedEnterIds_)
            {
                if (auto neighbor = GetPlayer(neighborId))
                {
                    neighbor->WriteToProto(ntfToSelf.add_entities());
                }
            }
            if (ntfToSelf.entities_size() > 0)
            {
                SendPacket(moverId, SC_ENTER_VIEW_NTF, ntfToSelf);
            }

            // B. 告诉这些新邻居："我进入了你们的视野"
            SCEnterViewNtf ntfToNeighbors;
            mover->WriteToProto(ntfToNeighbors.add_entities());
            std::string cachedBuffer = ntfToNeighbors.SerializeAsString();

            for (uint64_t neighborId : cachedEnterIds_)
            {
                SendBuffer(neighborId, SC_ENTER_VIEW_NTF, cachedBuffer);
            }
        }

        // 3. 处理 [Leave View]
        if (!cachedLeaveIds_.empty())
        {
            // A. 告诉 mover："这些邻居离开了你的视野"
            SCLeaveViewNtf ntfToSelf;
            for (uint64_t neighborId : cachedLeaveIds_)
            {
                ntfToSelf.add_entity_ids(neighborId);
            }
            if (ntfToSelf.entity_ids_size() > 0)
            {
                SendPacket(moverId, SC_LEAVE_VIEW_NTF, ntfToSelf);
            }

            // B. 告诉这些旧邻居："我离开了你们的视野"
            SCLeaveViewNtf ntfToNeighbors;
            ntfToNeighbors.add_entity_ids(moverId);
            std::string cachedBuffer = ntfToNeighbors.SerializeAsString();

            for (uint64_t neighborId : cachedLeaveIds_)
            {
                SendBuffer(neighborId, SC_LEAVE_VIEW_NTF, cachedBuffer);
            }
        }

        // 4. 处理 [Move] 广播
        std::vector<uint64_t> neighbors;
        neighbors.reserve(50);
        aoi_.GetViewEntityIds(newX, newY, neighbors);

        SCMoveNtf moveNtf;
        moveNtf.set_entity_id(moverId);
        auto *pos = moveNtf.mutable_pos();
        pos->set_x(newX);
        pos->set_y(newY);

        std::string moveBuffer = moveNtf.SerializeAsString();

        for (uint64_t neighborId : neighbors)
        {
            if (neighborId == moverId)
                continue;
            SendBuffer(neighborId, SC_MOVE_NTF, moveBuffer);
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
            // 这是线程安全的，因为 send_packet 只是把数据 Push 到 Connection 的 Outbox
            actor->send_packet(msgId, proto);
        }
    }

    void SceneActor::SendBuffer(uint64_t targetId, uint32_t msgId, const std::string &buffer)
    {
        if (auto actor = GetPlayer(targetId))
        {
            actor->send_buffer(msgId, buffer);
        }
    }

} // namespace aegis::core