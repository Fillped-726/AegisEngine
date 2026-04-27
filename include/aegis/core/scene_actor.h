/**
 * @file scene_actor.h
 * @brief Scene actor managing AOI grid, players, NPCs, and state sync.
 */
#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

#include "aegis/core/actor_traits.h"
#include "aegis/core/aoi_grid.h"
#include "aegis/core/message/message.h"
#include "aegis/core/playerActor.h"
#include "aegis/core/npc_actor.h"
#include "aegis/core/sync_manager.h"

namespace aegis::core
{

    // --- SceneActor ---
    /**
     * @brief Scene actor managing a game map instance.
     *
     * Owns AOI grid, player map, NPC map, and SyncManager.
     * Handles scene enter/leave/move/skill_cast messages.
     * Drives SyncManager::Tick() for state broadcast.
     * NPCs are only ticked when players are in AOI range.
     */
    class SceneActor : public PooledActor<SceneActor, 128, 32>
    {
    public:
        SceneActor(ActorID self_id, float width, float height, float cellSize);
        ~SceneActor() = default;

        void handle_message(ActorMessage *msg) override;
        void OnTick();

        void AddNpc(NpcActor *npc);
        void RemoveNpc(uint64_t raw_id);
        const AOIGrid &GetAoi() const { return aoi_; }
        PlayerActor *GetPlayer(uint64_t actorId) const;

    private:
        // 内部处理逻辑 (串行执行，无需加锁)
        void OnHandleEnter(SceneEnterMsg *msg);
        void OnHandleLeave(SceneLeaveMsg *msg);
        void OnHandleMove(SceneMoveMsg *msg);
        void OnHandleSkillCast(SceneSkillCastMsg *msg);

        void ProcessAoiEnterLeave(PlayerActor *mover,
                                  const std::vector<uint64_t> &enterIds,
                                  const std::vector<uint64_t> &leaveIds);

        template <typename T>
        void SendPacket(uint64_t targetId, uint32_t msgId, const T &proto);

        void SendSharedBuffer(uint64_t targetId, uint32_t msgId, std::shared_ptr<std::string> sharedBuf);

        void reset(ActorID self_id, float width, float height, float cellSize);

    private:
        bool is_ticking_ = false; // 场景是否已开始 Tick 驱动
        AOIGrid aoi_;
        std::unordered_map<uint64_t, PlayerActor *> actors_;
        std::unordered_map<uint64_t, NpcActor *> npcs_;

        SyncManager sync_mgr_;

        // 缓存复用 (在 Actor 模型下单线程访问，安全)
        std::vector<uint64_t> cachedEnterIds_;
        std::vector<uint64_t> cachedLeaveIds_;

        // 人数上报计数器（每 60 Tick = 3s 上报一次）
        uint32_t report_counter_ = 0;
    };

} // namespace aegis::core
