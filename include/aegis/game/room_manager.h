/**
 * @file room_manager.h
 * @brief Room manager for room lifecycle, camp assignment, and dungeon management.
 *
 * Extends RPC handlers with on_assign_camp() — receives AssignCampReq
 * from PlayerActor, creates a new SceneActor camp or returns an existing one.
 *
 * Now also maintains camp_metas_ cache for listing camps to clients,
 * and dungeon_scene_id_ for single-instance dungeon management.
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
     *
     * Also manages a single dungeon instance (房主建房模式).
     */
    class RoomManager : public SimpleActor<RoomManager>
    {
    public:
        using SimpleActor::SimpleActor;

        ~RoomManager() override = default;

        // 获取缓存的营地列表
        const std::unordered_map<uint64_t, CampMeta> &camp_metas() const { return camp_metas_; }

        // 测试用：获取销毁中的场景集合
        const std::unordered_set<uint64_t> &destroying_scenes() const { return destroying_scenes_; }

        // 测试用：注册一个测试营地（供单元测试使用）
        void TestRegisterCamp(uint64_t scene_actor_id, const std::string &name, int32_t players)
        {
            camp_scenes_[scene_actor_id] = name;
            CampMeta meta;
            meta.scene_actor_id = scene_actor_id;
            meta.camp_name = name;
            meta.current_players = players;
            meta.max_players = 20;
            camp_metas_[scene_actor_id] = meta;
        }

        // 测试用：直接触发人数上报处理（供单元测试使用）
        void TestOnPlayerCount(uint64_t scene_actor_id, int32_t count)
        {
            CampPlayerCountMsg msg(scene_actor_id, count);
            on_camp_player_count(msg);
        }

    protected:
        void handle_message(ActorMessage *msg) override;

    private:
        void on_create_room(const RPCCreateRoomMsg &msg);
        void on_terminate_room(const RPCTerminateRoomMsg &msg);
        void on_scene_died(uint64_t deceased_id, int reason);
        void on_assign_camp(const RPCAssignCampMsg &msg);
        void on_camp_player_count(const CampPlayerCountMsg &msg);

        // ── 副本管理 ──
        void on_create_dungeon(const RPCCreateDungeonMsg &msg);
        void on_join_dungeon(const RPCJoinDungeonMsg &msg);
        void on_leave_dungeon(const RPCLeaveDungeonMsg &msg);

    private:
        std::unordered_map<uint32_t, uint32_t> room_id_to_actor_;
        std::unordered_map<uint32_t, uint32_t> actor_to_room_id_;

        // 营地元数据缓存（SceneActor 定期上报更新）
        std::unordered_map<uint64_t, CampMeta> camp_metas_;

        // 旧版营地记录（兼容过渡）
        std::unordered_map<uint64_t, std::string> camp_scenes_;

        std::unordered_set<uint64_t> destroying_scenes_;

        // ── 副本（单实例房主建房模式） ──
        uint64_t dungeon_scene_id_ = 0;           // 当前副本实例（0=无）
        uint64_t dungeon_owner_id_ = 0;           // 房主的 player_uid
        std::unordered_set<uint64_t> dungeon_players_; // 副本内所有玩家
    };
}
