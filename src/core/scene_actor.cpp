#include "aegis/core/scene_actor.h"
#include "aegis/common/aegisLog.h"
#include "cs_battle.pb.h"
#include "ids.pb.h"

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
        // 1. [系统] 存入映射表 (用于发消息)
        ActorID actor_id = msg->actor_id;
        auto *actor = ActorRegistry::instance().get(actor_id);
        if (!actor)
            return; // 容错

        auto *player = static_cast<PlayerActor *>(actor);
        actors_[actor_id.raw] = player;

        // 2. [算法] 加入 AOI (使用 ActorID 保证唯一性)
        aoi_.Add(actor_id.raw, msg->x, msg->y);

        // 3. [业务] 广播视野
        std::vector<uint64_t> neighbor_ids;
        aoi_.GetViewEntityIds(msg->x, msg->y, neighbor_ids);

        Log::instance().debug("[Scene] Actor {} (UID: {}) entered. Neighbors: {}",
                              actor_id.raw, msg->player_id, neighbor_ids.size());

        if (neighbor_ids.empty())
            return;

        // A. 构造 [我看见了谁]
        SCEnterViewNtf ntf_to_me;

        // B. 构造 [谁看见了我]
        SCEnterViewNtf ntf_to_others;
        auto *my_ent = ntf_to_others.add_entities();
        my_ent->set_entity_id(msg->player_id); // [直接用消息里的 UID!]
        my_ent->mutable_pos()->set_x(msg->x);
        my_ent->mutable_pos()->set_y(msg->y);
        std::string buf_to_others = ntf_to_others.SerializeAsString();

        for (uint64_t neighbor_raw_id : neighbor_ids)
        {
            if (neighbor_raw_id == actor_id.raw)
                continue;

            // 查找邻居对象
            auto it = actors_.find(neighbor_raw_id);
            if (it != actors_.end())
            {
                PlayerActor *neighbor = it->second;

                // 填入发给我的包
                auto *ent = ntf_to_me.add_entities();
                ent->set_entity_id(neighbor->get_player_id()); // 邻居的 UID 还是得 Get 一下
                ent->mutable_pos()->set_x(neighbor->GetX());
                ent->mutable_pos()->set_y(neighbor->GetY());

                // 发送 [我来了] 给邻居
                // 假设你实现了 send_buffer，如果没有就用 send_packet
                neighbor->send_buffer(ids::SC_ENTER_VIEW, buf_to_others);
            }
        }

        if (ntf_to_me.entities_size() > 0)
        {
            player->send_packet(ids::SC_ENTER_VIEW, ntf_to_me);
        }
    }

    void SceneActor::OnHandleLeave(SceneLeaveMsg *msg)
    {
        // 1. [数据解包] 从新消息结构里拿数据
        ActorID actor_id = msg->actor_id; // 用于查找 PlayerActor 对象
        uint64_t uid = msg->player_id;    // 用于告诉客户端“谁走了”

        // 2. [查表] 确保玩家确实在场景里
        uint64_t raw_id = actor_id.raw;
        auto it = actors_.find(raw_id);
        if (it == actors_.end())
            return;

        // 获取玩家对象是为了拿坐标 (AOI 删除需要坐标)
        PlayerActor *player = it->second;
        float x = player->GetX();
        float y = player->GetY();

        // 3. [核心逻辑] 获取“目击者” (谁需要知道我走了？)
        // 必须在 Remove 之前或者由 Remove 返回这些邻居
        // 这里采用稳妥做法：先查周围的人，再删自己
        std::vector<uint64_t> neighbors;
        aoi_.GetViewEntityIds(x, y, neighbors);

        // 4. [算法] 从 AOI 移除
        // 假设你的 Remove 只需要 ID 和坐标
        aoi_.Remove(raw_id, x, y);

        // 5. [广播] 通知周围的邻居
        if (!neighbors.empty())
        {
            SCLeaveViewNtf ntf;
            ntf.add_entity_ids(uid); // 【重点】告诉客户端是 UID: 10001 走了
            std::string buf = ntf.SerializeAsString();

            for (uint64_t neighbor_raw_id : neighbors)
            {
                // 排除自己 (因为自己马上要销毁/离开了，客户端通常自己处理自己的销毁)
                if (neighbor_raw_id == raw_id)
                    continue;

                // 发送给邻居
                // 注意：这里需要 neighbor_raw_id 也是 ActorID，去 actors_ 表里查对象
                if (auto neighbor_it = actors_.find(neighbor_raw_id); neighbor_it != actors_.end())
                {
                    // 使用 send_buffer 发送序列化好的数据
                    neighbor_it->second->send_buffer(ids::SC_LEAVE_VIEW, buf);
                }
            }
        }

        // 6. [清理] 从内存映射中移除
        actors_.erase(it);
        Log::instance().debug("[Scene] Actor {} (UID: {}) left.", raw_id, uid);
    }

    void SceneActor::OnHandleMove(SceneMoveMsg *msg)
    {
        // 1. [数据准备] 区分系统ID和业务ID
        uint64_t mover_actor_id = msg->actor_id.raw; // 用于查表、AOI
        uint64_t mover_uid = msg->player_id;         // 用于发包给客户端

        float oldX = msg->oldX;
        float oldY = msg->oldY;
        float newX = msg->newX;
        float newY = msg->newY;

        // 2. [AOI 计算] 使用 ActorID 进行内部计算
        // cachedEnterIds_ 和 cachedLeaveIds_ 里存的都是 ActorID
        bool moved = aoi_.Move(mover_actor_id, oldX, oldY, newX, newY, cachedEnterIds_, cachedLeaveIds_);

        if (!moved)
            return;

        auto mover = GetPlayer(mover_actor_id);
        if (!mover)
            return;

        // 更新一下 mover 自己的坐标缓存
        mover->SetPos(newX, newY);

        // =========================================================
        // 3. 处理 [Enter View] (遇见了新朋友)
        // =========================================================
        if (!cachedEnterIds_.empty())
        {
            // A. 告诉 mover (我自己)："你看见了这些新邻居"
            SCEnterViewNtf ntfToSelf;

            // B. 告诉这些新邻居 (别人)："我(UID)进入了你们的视野"
            SCEnterViewNtf ntfToNeighbors;
            auto *me = ntfToNeighbors.add_entities();
            me->set_entity_id(mover_uid); // [重点] 发 UID
            me->mutable_pos()->set_x(newX);
            me->mutable_pos()->set_y(newY);
            std::string bufToNeighbors = ntfToNeighbors.SerializeAsString();

            for (uint64_t neighborActorId : cachedEnterIds_)
            {
                if (auto neighbor = GetPlayer(neighborActorId))
                {
                    // 填入发给我的包 (把邻居的 ActorID 转为 UID)
                    auto *ent = ntfToSelf.add_entities();
                    ent->set_entity_id(neighbor->get_player_id()); // [重点] 邻居 UID
                    ent->mutable_pos()->set_x(neighbor->GetX());
                    ent->mutable_pos()->set_y(neighbor->GetY());

                    // 发给邻居
                    SendBuffer(neighborActorId, ids::SC_ENTER_VIEW, bufToNeighbors);
                }
            }

            if (ntfToSelf.entities_size() > 0)
            {
                SendPacket(mover_actor_id, ids::SC_ENTER_VIEW, ntfToSelf);
            }
        }

        // =========================================================
        // 4. 处理 [Leave View] (朋友离开了)
        // =========================================================
        if (!cachedLeaveIds_.empty())
        {
            // A. 告诉 mover："这些邻居(UID)离开了你的视野"
            SCLeaveViewNtf ntfToSelf;

            // B. 告诉这些旧邻居："我(UID)离开了你们的视野"
            SCLeaveViewNtf ntfToNeighbors;
            ntfToNeighbors.add_entity_ids(mover_uid); // [重点] 发 UID
            std::string bufToNeighbors = ntfToNeighbors.SerializeAsString();

            for (uint64_t neighborActorId : cachedLeaveIds_)
            {
                // 注意：虽然离开了视野，但 neighbor 只要还在场景里，GetPlayer 就能取到
                if (auto neighbor = GetPlayer(neighborActorId))
                {
                    // 填入发给我的包 (把邻居 ActorID 转为 UID)
                    ntfToSelf.add_entity_ids(neighbor->get_player_id()); // [重点] 邻居 UID

                    // 发给邻居
                    SendBuffer(neighborActorId, ids::SC_LEAVE_VIEW, bufToNeighbors);
                }
            }

            if (ntfToSelf.entity_ids_size() > 0)
            {
                SendPacket(mover_actor_id, ids::SC_LEAVE_VIEW, ntfToSelf);
            }
        }

        // =========================================================
        // 5. 处理 [Move] 广播 (视野内移动)
        // =========================================================
        std::vector<uint64_t> neighbors;
        neighbors.reserve(50);
        // 获取当前视野内所有的 ActorID
        aoi_.GetViewEntityIds(newX, newY, neighbors);

        SCMoveNtf moveNtf;
        moveNtf.set_entity_id(mover_uid); // [重点] 发 UID
        auto *pos = moveNtf.mutable_pos();
        pos->set_x(newX);
        pos->set_y(newY);

        std::string moveBuffer = moveNtf.SerializeAsString();

        for (uint64_t neighborActorId : neighbors)
        {
            if (neighborActorId == mover_actor_id)
                continue; // 不发给自己(通常客户端自己预测)

            // 简单发送 (此处包含了刚才 Enter/Leave 的人，会有冗余包，
            // 但为了代码简单且健壮，先这样发，客户端能处理冗余 Move)
            SendBuffer(neighborActorId, ids::SC_MOVE_NTF, moveBuffer);
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