/**
 * @file room_manager.h
 * @brief Room manager for room lifecycle and camp assignment (Module C).
 *
 * Extends RPC handlers with on_assign_camp() — receives AssignCampReq
 * from PlayerActor, creates a new SceneActor camp or returns an existing one.
 *
 * Now also maintains camp_metas_ cache for listing camps to clients.
 */
#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_set>
#include "aegis/core/actor_traits.h"
#include "aegis/core/message/message_rpc.h"
#include "aegis/core/actor_registry.h"

#include "cs_battle.pb.h"

namespace aegis::core
{
    // ── 营地元数据缓存 ──
    struct CampMeta
    {
        uint64_t scene_actor_id = 0;
        std::string camp_name;
        int32_t current_players = 0;
        int32_t max_players = 20;
    };

    // ── 营地人数上报消息（SceneActor → RoomManager, Fire-and-Forget） ──
    struct CampPlayerCountMsg : public BasicMessage<CampPlayerCountMsg, MSG_TYPE_CAMP_PLAYER_COUNT>
    {
        uint64_t scene_actor_id = 0;
        int32_t player_count = 0;

        CampPlayerCountMsg(uint64_t id, int32_t count)
            : scene_actor_id(id), player_count(count) {}

        void finalize() { delete this; }
    };

    /**
     * @brief Room lifecycle and camp assignment manager.
     */
    class RoomManager : public SimpleActor<RoomManager>
    {
    public:
        using SimpleActor::SimpleActor;

        ~RoomManager() override = default;

        // 获取缓存的营地列表
        const std::unordered_map<uint64_t, CampMeta> &camp_metas() const { return camp_metas_; }

    protected:
        void handle_message(ActorMessage *msg) override;

    private:
        void on_create_room(const RPCCreateRoomMsg &msg);
        void on_terminate_room(const RPCTerminateRoomMsg &msg);
        void on_scene_died(uint64_t deceased_id, int reason);
        void on_assign_camp(const RPCAssignCampMsg &msg);
        void on_camp_player_count(const CampPlayerCountMsg &msg);

    private:
        std::unordered_map<uint32_t, uint32_t> room_id_to_actor_;
        std::unordered_map<uint32_t, uint32_t> actor_to_room_id_;

        // 营地元数据缓存（SceneActor 定期上报更新）
        std::unordered_map<uint64_t, CampMeta> camp_metas_;

        // 旧版营地记录（兼容过渡）
        std::unordered_map<uint64_t, std::string> camp_scenes_;

        std::unordered_set<uint64_t> destroying_scenes_;
    };
}
