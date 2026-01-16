#include "aegis/core/scene.h"
// 引入生成的 Protobuf 头文件
#include "scene.pb.h"

namespace aegis::core
{
    using namespace aegis::protocol;
    using namespace aegis::common;

    // [Config] 协议 ID 定义 (与客户端约定的常量)
    constexpr uint32_t MSG_SC_MOVE_NTF = 1002;
    constexpr uint32_t MSG_SC_ENTER_VIEW_NTF = 1003;
    constexpr uint32_t MSG_SC_LEAVE_VIEW_NTF = 1004;

    Scene::Scene(float width, float height, float cellSize)
        : aoi_(width, height, cellSize)
    {
        cachedEnterIds_.reserve(50);
        cachedLeaveIds_.reserve(50);
        Log::instance().info("[Scene] Initialized Grid: {}x{} (Cell: {})", width, height, cellSize);
    }

    void Scene::AddPlayer(PlayerActor *actor)
    {
        if (!actor)
            return;

        uint64_t id = actor->GetID();
        actors_[id] = actor;

        // 同步位置到 AOI 网格
        aoi_.Add(id, actor->GetX(), actor->GetY());

        Log::instance().debug("[Scene] Player {} added at ({}, {})", id, actor->GetX(), actor->GetY());
    }

    void Scene::RemovePlayer(uint64_t actorId)
    {
        auto it = actors_.find(actorId);
        if (it != actors_.end())
        {
            PlayerActor *actor = it->second;
            // 从 AOI 网格移除
            aoi_.Remove(actorId, actor->GetX(), actor->GetY());
            actors_.erase(it);
        }
    }

    PlayerActor *Scene::GetPlayer(uint64_t actorId) const
    {
        auto it = actors_.find(actorId);
        return (it != actors_.end()) ? it->second : nullptr;
    }

    // =========================================================
    // 核心逻辑：OnPlayerMove (真实网络版)
    // =========================================================
    void Scene::OnPlayerMove(PlayerActor *mover, float newX, float newY)
    {
        if (!mover)
            return;

        uint64_t moverId = mover->GetID();
        float oldX = mover->GetX();
        float oldY = mover->GetY();

        // 1. AOI 计算差分
        bool moved = aoi_.Move(moverId, oldX, oldY, newX, newY, cachedEnterIds_, cachedLeaveIds_);
        if (!moved)
            return; // 位置未变或非法

        // 更新 Actor 自身的坐标属性 (重要！否则下次 Move 还是用旧坐标)
        mover->SetPos(newX, newY);

        // 2. 处理 [Enter View]
        // -------------------------------------------------
        if (!cachedEnterIds_.empty())
        {
            // A. 告诉 mover (我自己)："你看见了这些新邻居"
            SCEnterViewNtf ntfToSelf;
            for (uint64_t neighborId : cachedEnterIds_)
            {
                if (auto *neighbor = GetPlayer(neighborId))
                {
                    // 调用 PlayerActor::WriteToProto 填充数据
                    neighbor->WriteToProto(ntfToSelf.add_entities());
                }
            }
            if (ntfToSelf.entities_size() > 0)
            {
                // [Real] 发送 Protobuf 包
                SendPacket(moverId, MSG_SC_ENTER_VIEW_NTF, ntfToSelf);
            }

            // B. 告诉这些新邻居："我进入了你们的视野"
            // [Optimization] 序列化复用：只序列化一次，广播 Buffer
            SCEnterViewNtf ntfToNeighbors;
            mover->WriteToProto(ntfToNeighbors.add_entities());

            std::string cachedBuffer = ntfToNeighbors.SerializeAsString();

            for (uint64_t neighborId : cachedEnterIds_)
            {
                // [Real] 发送 Raw Buffer
                SendBuffer(neighborId, MSG_SC_ENTER_VIEW_NTF, cachedBuffer);
            }
        }

        // 3. 处理 [Leave View]
        // -------------------------------------------------
        if (!cachedLeaveIds_.empty())
        {
            // A. 告诉 mover (我自己)："这些邻居离开了你的视野"
            SCLeaveViewNtf ntfToSelf;
            for (uint64_t neighborId : cachedLeaveIds_)
            {
                ntfToSelf.add_entity_ids(neighborId);
            }
            if (ntfToSelf.entity_ids_size() > 0)
            {
                SendPacket(moverId, MSG_SC_LEAVE_VIEW_NTF, ntfToSelf);
            }

            // B. 告诉这些旧邻居："我离开了你们的视野"
            SCLeaveViewNtf ntfToNeighbors;
            ntfToNeighbors.add_entity_ids(moverId);

            std::string cachedBuffer = ntfToNeighbors.SerializeAsString();

            for (uint64_t neighborId : cachedLeaveIds_)
            {
                SendBuffer(neighborId, MSG_SC_LEAVE_VIEW_NTF, cachedBuffer);
            }
        }

        // 4. 处理 [Move] 广播
        // -------------------------------------------------
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
            SendBuffer(neighborId, MSG_SC_MOVE_NTF, moveBuffer);
        }
    }

    // =========================================================
    // Network Helpers Implementation
    // =========================================================

    // 这里必须在 .cpp 中实现，因为我们在 .h 中声明了它
    // 只要 Scene::OnPlayerMove 在这个文件里实例化了它，连接器就能找到符号
    template <typename T>
    void Scene::SendPacket(uint64_t targetId, uint32_t msgId, const T &proto)
    {
        if (auto *actor = GetPlayer(targetId))
        {
            // [Real] 调用 PlayerActor 的真实接口
            actor->send_packet(msgId, proto);
        }
    }

    void Scene::SendBuffer(uint64_t targetId, uint32_t msgId, const std::string &buffer)
    {
        if (auto *actor = GetPlayer(targetId))
        {
            // [Real] 调用 PlayerActor 的真实接口
            actor->send_buffer(msgId, buffer);
        }
    }

} // namespace aegis::core