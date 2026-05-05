// room_manager.cpp

#include "aegis/game/room_manager.h"
#include "aegis/common/aegisLog.h"
#include "aegis/game/scene_actor.h"
#include "aegis/game/npc_actor.h"
#include "aegis/core/message/message.h"
#include "aegis/common/actor_utils.h"

namespace aegis::core
{

    void RoomManager::handle_message(ActorMessage *msg)
    {
        switch (msg->type_id)
        {
        // 1. 创建房间请求 (RPC)
        case MSG_TYPE_RPC_CREATE_ROOM:
        {
            auto *real_msg = static_cast<RPCCreateRoomMsg *>(msg);
            on_create_room(*real_msg);
            break;
        }

        // 2. 销毁房间请求 (RPC)
        case MSG_TYPE_RPC_TERMINATE_ROOM:
        {
            auto *real_msg = static_cast<RPCTerminateRoomMsg *>(msg);
            on_terminate_room(*real_msg);
            break;
        }

        // 3. 监管：收到子节点死亡通知
        case MSG_TYPE_ACTOR_DIED:
        {
            auto *real_msg = static_cast<ActorDiedMsg *>(msg);
            on_scene_died(real_msg->deceased_id.raw, real_msg->reason);
            break;
        }

        // 4. 营地分配请求 (Module C)
        case MSG_TYPE_RPC_ASSIGN_CAMP:
        {
            auto *real_msg = static_cast<RPCAssignCampMsg *>(msg);
            on_assign_camp(*real_msg);
            break;
        }

        // 5. 营地人数上报（SceneActor Fire-and-Forget）
        case MSG_TYPE_CAMP_PLAYER_COUNT:
        {
            auto *real_msg = static_cast<CampPlayerCountMsg *>(msg);
            on_camp_player_count(*real_msg);
            break;
        }

        // 6. 副本 RPC
        case MSG_TYPE_RPC_CREATE_DUNGEON:
        {
            auto *real_msg = static_cast<RPCCreateDungeonMsg *>(msg);
            on_create_dungeon(*real_msg);
            break;
        }

        case MSG_TYPE_RPC_JOIN_DUNGEON:
        {
            auto *real_msg = static_cast<RPCJoinDungeonMsg *>(msg);
            on_join_dungeon(*real_msg);
            break;
        }

        case MSG_TYPE_RPC_LEAVE_DUNGEON:
        {
            auto *real_msg = static_cast<RPCLeaveDungeonMsg *>(msg);
            on_leave_dungeon(*real_msg);
            break;
        }

        default:
            aegis::Log::instance().warn("[RoomMgr] Unknown message type: {}", msg->type_id);
            break;
        }
    }

    void RoomManager::on_create_room(const RPCCreateRoomMsg &msg)
    {
        const auto &req = msg.req;
        uint32_t room_id = req.room_id();

        aegis::ss::bridge::CreateRoomRes res;

        // 1. 查重
        if (room_id_to_actor_.find(room_id) != room_id_to_actor_.end())
        {
            res.mutable_header()->set_code(1);
            res.mutable_header()->set_msg("Room ID already exists");
            msg.Reply(res);
            return;
        }

        // 2. 孵化 SceneActor（使用与默认场景一致的地图配置：2000x2000, cell=256）
        ActorID scene_id = ActorRegistry::instance().create_actor<SceneActor>(-1000.0f, -1000.0f, 1000.0f, 1000.0f, 256.0f);

        if (!scene_id.is_valid())
        {
            res.mutable_header()->set_code(2);
            res.mutable_header()->set_msg("Failed to allocate SceneActor (Out of memory/slots)");
            msg.Reply(res);
            return;
        }

        // 3. 认父
        auto *scene = ActorRegistry::instance().get(scene_id);
        if (scene)
        {
            scene->set_parent_id(this->id());
        }
        else
        {
            res.mutable_header()->set_code(3);
            res.mutable_header()->set_msg("System Error: Scene created but not found");
            msg.Reply(res);
            return;
        }

        // 4. 记录状态
        room_id_to_actor_[room_id] = scene_id.raw;
        actor_to_room_id_[scene_id.raw] = room_id;

        aegis::Log::instance().info("[RoomMgr] Created Room {} -> SceneActor {}", room_id, scene_id.raw);

        // 5. 回复 RPC
        res.mutable_header()->set_code(0);
        res.mutable_header()->set_msg("Room created");
        res.set_room_id(room_id);
        msg.Reply(res);
    }

    void RoomManager::on_terminate_room(const RPCTerminateRoomMsg &msg)
    {
        // 简化处理
        const auto &req = msg.req;
        uint32_t room_id = req.room_id();

        auto it = room_id_to_actor_.find(room_id);
        if (it == room_id_to_actor_.end())
        {
            aegis::Log::instance().warn("[RoomMgr] Room {} not found for termination.", room_id);
            return;
        }

        uint64_t scene_raw_id = it->second;
        ActorID scene_id{scene_raw_id};

        // 从注册表移除并标记销毁
        destroying_scenes_.insert(scene_raw_id);
        ActorRegistry::instance().remove(scene_id);

        room_id_to_actor_.erase(it);
        actor_to_room_id_.erase(scene_raw_id);

        aegis::Log::instance().info("[RoomMgr] Terminated Room {} (SceneActor {})", room_id, scene_raw_id);
    }

    void RoomManager::on_scene_died(uint64_t deceased_id, int reason)
    {
        aegis::Log::instance().info("[RoomMgr] SceneActor {} died (reason={})", deceased_id, reason);

        auto it = actor_to_room_id_.find(deceased_id);
        if (it != actor_to_room_id_.end())
        {
            uint32_t room_id = it->second;
            room_id_to_actor_.erase(room_id);
            actor_to_room_id_.erase(it);
        }

        // 如果是副本场景，清理副本状态
        if (deceased_id == dungeon_scene_id_)
        {
            aegis::Log::instance().info("[RoomMgr] Dungeon scene {} destroyed, clearing dungeon state.", deceased_id);
            dungeon_scene_id_ = 0;
            dungeon_owner_id_ = 0;
            dungeon_players_.clear();
        }

        destroying_scenes_.erase(deceased_id);
    }

    // ════════════════════════════════════════════════════
    // 营地分配（已实现）
    // ════════════════════════════════════════════════════

    void RoomManager::on_assign_camp(const RPCAssignCampMsg &msg)
    {
        const auto &req = msg.req;

        AssignCampRes result;
        result.ret_code = 0;
        result.scene_actor_id = ActorID{0};

        if (req.is_create)
        {
            // 1. 创建新营地场景
            ActorID scene_id = ActorRegistry::instance().create_actor<SceneActor>(
                -1000.0f, -1000.0f, 1000.0f, 1000.0f, 256.0f);

            if (!scene_id.is_valid())
            {
                result.ret_code = -1;
                result.err_msg = "Failed to allocate scene";
                msg.Reply(result);
                return;
            }

            auto *scene = ActorRegistry::instance().get(scene_id);
            if (!scene)
            {
                result.ret_code = -2;
                result.err_msg = "Scene created but not found";
                msg.Reply(result);
                return;
            }

            scene->set_parent_id(this->id());
            scene->set_worker_id(Worker::get_current_id());

            // 保存营地元数据
            camp_scenes_[scene_id.raw] = req.camp_name;
            CampMeta meta;
            meta.scene_actor_id = scene_id.raw;
            meta.camp_name = req.camp_name;
            meta.current_players = 1;
            meta.max_players = 20;
            camp_metas_[scene_id.raw] = meta;

            result.scene_actor_id = scene_id;
            result.camp_name = req.camp_name;

            aegis::Log::instance().info("[RoomMgr] Created camp '{}' -> SceneActor {}",
                                         req.camp_name, scene_id.raw);
        }
        else
        {
            // 加入营地
            uint64_t target_id = req.target_scene_id;
            auto it = camp_metas_.find(target_id);
            if (it == camp_metas_.end())
            {
                result.ret_code = -3;
                result.err_msg = "Camp not found";
                msg.Reply(result);
                return;
            }

            auto &meta = it->second;
            if (meta.current_players >= meta.max_players)
            {
                result.ret_code = -4;
                result.err_msg = "Camp is full";
                msg.Reply(result);
                return;
            }

            meta.current_players++;
            result.scene_actor_id = ActorID{target_id};
            result.camp_name = meta.camp_name;

            aegis::Log::instance().info("[RoomMgr] Player {} joined camp '{}' (SceneActor {})",
                                         req.player_uid, meta.camp_name, target_id);
        }

        msg.Reply(result);
    }

    void RoomManager::on_camp_player_count(const CampPlayerCountMsg &msg)
    {
        auto it = camp_metas_.find(msg.scene_actor_id);
        if (it != camp_metas_.end())
        {
            it->second.current_players = msg.player_count;
        }
    }

    // ════════════════════════════════════════════════════
    // 副本管理
    // ════════════════════════════════════════════════════

    void RoomManager::on_create_dungeon(const RPCCreateDungeonMsg &msg)
    {
        const auto &req = msg.req;

        CreateDungeonRes result;
        result.ret_code = 0;

        // 1. 检查是否已有副本实例
        if (dungeon_scene_id_ != 0)
        {
            // 已有副本，让玩家加入
            auto *existing = ActorRegistry::instance().get(ActorID{dungeon_scene_id_});
            if (existing)
            {
                result.dungeon_scene_id = ActorID{dungeon_scene_id_};
                result.ret_code = 0;
                result.err_msg = "Joined existing dungeon";
                msg.Reply(result);
                return;
            }
            else
            {
                // 残留的副本ID，清理
                aegis::Log::instance().warn("[RoomMgr] Cleaning up stale dungeon scene ID: {}", dungeon_scene_id_);
                dungeon_scene_id_ = 0;
                dungeon_owner_id_ = 0;
                dungeon_players_.clear();
            }
        }

        // 2. 创建副本 SceneActor
        ActorID scene_id = ActorRegistry::instance().create_actor<SceneActor>(
            -500.0f, -500.0f, 500.0f, 500.0f, 256.0f);

        if (!scene_id.is_valid())
        {
            result.ret_code = -1;
            result.err_msg = "Failed to allocate dungeon scene";
            msg.Reply(result);
            return;
        }

        auto *scene = ActorRegistry::instance().get(scene_id);
        if (!scene)
        {
            result.ret_code = -2;
            result.err_msg = "Dungeon scene created but not found";
            msg.Reply(result);
            return;
        }

        scene->set_parent_id(this->id());
        scene->set_worker_id(Worker::get_current_id());

        // 3. 记录副本状态
        dungeon_scene_id_ = scene_id.raw;
        dungeon_owner_id_ = req.player_uid;

        // 4. 在副本中刷怪（5 只史莱姆 NPC）
        auto *dungeon_scene = static_cast<SceneActor *>(scene);
        float spawn_positions[5][2] = {
            {100.0f, 100.0f},
            {-100.0f, 100.0f},
            {100.0f, -100.0f},
            {-100.0f, -100.0f},
            {0.0f, 150.0f}
        };

        for (int i = 0; i < 5; i++)
        {
            ActorID npc_id = ActorRegistry::instance().create_actor<NpcActor>();
            if (npc_id.is_valid())
            {
                auto *npc = static_cast<NpcActor *>(ActorRegistry::instance().get(npc_id));
                if (npc)
                {
                    npc->reset(npc_id, spawn_positions[i][0], spawn_positions[i][1]);
                    npc->SetScene(dungeon_scene);
                    npc->set_parent_id(scene_id);
                    npc->SetAiActive(true);
                    dungeon_scene->AddNpc(npc);
                }
            }
        }

        aegis::Log::instance().info("[RoomMgr] Created dungeon (SceneActor {}) with 5 monsters, owner={}",
                                     scene_id.raw, req.player_uid);

        result.dungeon_scene_id = scene_id;
        result.ret_code = 0;
        result.err_msg = "Dungeon created";
        msg.Reply(result);
    }

    void RoomManager::on_join_dungeon(const RPCJoinDungeonMsg &msg)
    {
        const auto &req = msg.req;

        JoinDungeonRes result;
        result.ret_code = 0;

        if (dungeon_scene_id_ == 0)
        {
            result.ret_code = -1;
            result.err_msg = "No dungeon available";
            msg.Reply(result);
            return;
        }

        auto *scene = ActorRegistry::instance().get(ActorID{dungeon_scene_id_});
        if (!scene)
        {
            result.ret_code = -2;
            result.err_msg = "Dungeon scene not found";
            dungeon_scene_id_ = 0;
            dungeon_owner_id_ = 0;
            dungeon_players_.clear();
            msg.Reply(result);
            return;
        }

        result.dungeon_scene_id = ActorID{dungeon_scene_id_};
        result.ret_code = 0;
        msg.Reply(result);
    }

    void RoomManager::on_leave_dungeon(const RPCLeaveDungeonMsg &msg)
    {
        const auto &req = msg.req;
        LeaveDungeonRes result;
        result.ret_code = 0;

        aegis::Log::instance().info("[RoomMgr] Player {} leaving dungeon (owner={}, is_owner={})",
                                     req.player_uid, dungeon_owner_id_, req.is_owner);

        if (req.is_owner || req.player_uid == dungeon_owner_id_)
        {
            // 房主离开 → 销毁副本
            if (dungeon_scene_id_ != 0)
            {
                ActorID scene_id{dungeon_scene_id_};
                destroying_scenes_.insert(dungeon_scene_id_);
                ActorRegistry::instance().remove(scene_id);
                aegis::Log::instance().info("[RoomMgr] Dungeon destroyed by owner leave. SceneActor: {}",
                                             dungeon_scene_id_);
            }

            dungeon_scene_id_ = 0;
            dungeon_owner_id_ = 0;
            dungeon_players_.clear();
        }

        msg.Reply(result);
    }

} // namespace aegis::core
