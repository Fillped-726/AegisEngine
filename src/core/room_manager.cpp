// room_manager.cpp

#include "aegis/core/room_manager.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/scene_actor.h"
#include "aegis/core/message/message.h"

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

        // 2. 孵化 SceneActor
        ActorID scene_id = ActorRegistry::instance().create_actor<SceneActor>(500.0f, 500.0f, 10.0f);

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
        res.set_room_id(room_id);
        msg.Reply(res);
    }

    void RoomManager::on_terminate_room(const RPCTerminateRoomMsg &msg)
    {
        uint32_t room_id = msg.req.room_id();
        auto it = room_id_to_actor_.find(room_id);

        aegis::ss::bridge::TerminateRoomRes res;

        if (it != room_id_to_actor_.end())
        {
            uint32_t scene_actor_id = it->second;

            room_id_to_actor_.erase(it);
            actor_to_room_id_.erase(scene_actor_id);

            auto *scene_ptr = ActorRegistry::instance().get(scene_actor_id);
            if (scene_ptr)
            {
                scene_ptr->push(new ActorDestroyMsg());
            }

            res.mutable_header()->set_code(0);
        }
        else
        {
            res.mutable_header()->set_code(1);
            res.mutable_header()->set_msg("Room not found");
        }

        msg.Reply(res);
    }

    void RoomManager::on_scene_died(uint64_t deceased_id, int reason)
    {
        // --- 1. 清理营地相关状态 ---
        // 无论是因为 0 人销毁还是崩溃，都要清理
        destroying_scenes_.erase(deceased_id);
        camp_metas_.erase(deceased_id);
        camp_scenes_.erase(deceased_id);

        // --- 2. 清理房间映射 (使用 uint64_t 避免截断) ---
        auto it = actor_to_room_id_.find(deceased_id);
        if (it != actor_to_room_id_.end())
        {
            uint32_t room_id = it->second;

            // 双向解除绑定
            room_id_to_actor_.erase(room_id);
            actor_to_room_id_.erase(it);

            if (reason != 0)
            {
                aegis::Log::instance().error("[RoomMgr] Room {} (ActorID: {}) CRASHED! Reason: {}",
                                             room_id, deceased_id, reason);
            }
            else
            {
                aegis::Log::instance().info("[RoomMgr] Room {} (ActorID: {}) cleanup complete.",
                                            room_id, deceased_id);
            }
        }
        else
        {
            // 如果不是房间，可能是纯营地场景
            aegis::Log::instance().info("[RoomMgr] Camp Scene {} fully reclaimed.", deceased_id);
        }
    }

    void RoomManager::on_assign_camp(const RPCAssignCampMsg &msg)
    {
        const auto &req = msg.req;
        AssignCampRes res;

        if (req.is_create)
        {
            // --------------------------------------------------
            // 创建新营地
            // --------------------------------------------------
            ActorID camp_id = ActorRegistry::instance().create_actor<SceneActor>(500.0f, 500.0f, 10.0f);
            if (!camp_id.is_valid())
            {
                res.ret_code = 1;
                res.err_msg = "Failed to create camp scene";
                msg.Reply(res);
                return;
            }

            auto *scene = ActorRegistry::instance().get(camp_id);
            if (scene)
            {
                scene->set_parent_id(this->id());
            }

            // 记录营地
            std::string name = req.camp_name.empty() ? "Camp_" + std::to_string(camp_id.raw) : req.camp_name;
            camp_scenes_[camp_id.raw] = name;

            // 同步写入元数据缓存（初始 1 人：创建者自己）
            CampMeta meta;
            meta.scene_actor_id = camp_id.raw;
            meta.camp_name = name;
            meta.current_players = 1;
            meta.max_players = 20;
            camp_metas_[camp_id.raw] = meta;

            res.ret_code = 0;
            res.scene_actor_id = camp_id;
            res.camp_name = name;

            aegis::Log::instance().info("[Camp] Player {} created camp '{}' -> SceneActor {}",
                                        req.player_uid, name, camp_id.raw);
        }
        else
        {
            // --------------------------------------------------
            // 加入已有营地
            // --------------------------------------------------
            uint64_t target_scene = req.target_scene_id;

            // 验证存在
            auto it = camp_scenes_.find(target_scene);
            if (it != camp_scenes_.end())
            {
                // 二次校验：检查人数是否已满
                auto meta_it = camp_metas_.find(target_scene);
                if (meta_it != camp_metas_.end() &&
                    meta_it->second.current_players >= meta_it->second.max_players)
                {
                    res.ret_code = 4;
                    res.err_msg = "Camp is full";
                    aegis::Log::instance().warn("[Camp] Player {} failed to join camp '{}': full",
                                                req.player_uid, it->second);
                }
                else
                {
                    auto *scene = ActorRegistry::instance().get(target_scene);
                    if (scene)
                    {
                        res.ret_code = 0;
                        res.scene_actor_id = ActorID(target_scene);
                        res.camp_name = it->second;

                        // 人数+1（最终由 SceneActor 上报校正）
                        if (meta_it != camp_metas_.end())
                            meta_it->second.current_players++;

                        aegis::Log::instance().info("[Camp] Player {} joining camp '{}' (SceneActor {})",
                                                    req.player_uid, it->second, target_scene);
                    }
                    else
                    {
                        // 营地已销毁但记录还在，清理并报错
                        camp_scenes_.erase(it);
                        res.ret_code = 2;
                        res.err_msg = "Camp scene no longer exists";
                    }
                }
            }
            else
            {
                res.ret_code = 3;
                res.err_msg = "Camp not found";
            }
        }

        msg.Reply(res);
    }

    void RoomManager::on_camp_player_count(const CampPlayerCountMsg &msg)
    {
        // --- 1. 幂等性拦截 ---
        // 如果该场景已经在销毁流程中，直接无视后续所有上报，防止日志复读
        if (destroying_scenes_.find(msg.scene_actor_id) != destroying_scenes_.end())
        {
            return;
        }

        if (msg.player_count <= 0)
        {
            // --- 2. 触发销毁逻辑 ---
            auto *scene = ActorRegistry::instance().get(msg.scene_actor_id);
            if (scene)
            {
                aegis::Log::instance().info("[RoomMgr] Camp {} is empty. Terminal sequence initiated.",
                                            msg.scene_actor_id);

                // 标记为“销毁中”，拦截后续上报
                destroying_scenes_.insert(msg.scene_actor_id);

                // 发送销毁指令
                auto *destroy_msg = new ActorDestroyMsg();
                scene->push(destroy_msg);
            }
            return;
        }

        // --- 3. 正常人数更新 ---
        auto it = camp_metas_.find(msg.scene_actor_id);
        if (it != camp_metas_.end())
        {
            it->second.current_players = msg.player_count;
        }
        else
        {
            // 自动恢复逻辑：如果 meta 丢失但记录还在，重新填充（常用于热更或意外丢包后的状态重建）
            auto scene_it = camp_scenes_.find(msg.scene_actor_id);
            if (scene_it != camp_scenes_.end())
            {
                CampMeta meta;
                meta.scene_actor_id = msg.scene_actor_id;
                meta.camp_name = scene_it->second;
                meta.current_players = msg.player_count;
                meta.max_players = 20;
                camp_metas_[msg.scene_actor_id] = meta;
            }
        }
    }

    // 更新 on_assign_camp 中创建营地时写入 camp_metas_
    // 注：在 on_assign_camp 的创建分支中添加缓存写入
    // onCreate already writes to camp_scenes_; we add camp_metas_ sync here
    // The actual modification is done below in the create branch

} // namespace aegis::core
