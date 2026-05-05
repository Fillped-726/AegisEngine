#include "handler_loader.h"

// Core framework
#include "aegis/net/dispatcher.h"
#include "aegis/game/playerActor.h"
#include "aegis/core/scheduler.h"
#include "aegis/core/actor_registry.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/message/message.h"
#include "aegis/core/worker.h"
#include "aegis/common/actor_utils.h"

// Protocol Buffers
#include "cs_lobby.pb.h"
#include "cs_battle.pb.h"
#include "cs_dungeon.pb.h"
#include "ids.pb.h"

// GameApp — centralized business bootstrap
#include "aegis/game/game_app.h"

// RoomManager — 营地缓存查询
#include "aegis/game/room_manager.h"

// RPC 协程支持
#include "aegis/game/rpc_awaiter.h"

using namespace aegis::cs::lobby;
using namespace aegis::cs::battle;
using namespace aegis::ids;
using namespace aegis::core;

namespace aegis::gate
{
    void load_handlers()
    {
        auto &d = aegis::net::Dispatcher::instance();

        // ==========================================================
        // Handler 1: Login (登录并进入主城)
        // ==========================================================
        d.register_handler<LoginReq>(
            ids::CS_LOGIN_REQ,
            [](aegis::core::Actor *actor, const LoginReq &req) -> aegis::core::Task<void>
            {
                auto player = dynamic_cast<aegis::core::PlayerActor *>(actor);
                if (!player) [[unlikely]]
                {
                    Log::instance().error("[Logic] Handler called with invalid actor type");
                    co_return;
                }

                uint64_t uid = req.uid();

                // uid 字段已弃用，统一使用 ActorID 标识玩家
                Log::instance().info("[Logic] Login Request | ActorID: {} (client uid: {})", player->id().raw, uid);

                // 2. 出生点计算
                float spawnX = 0.0f;
                float spawnY = 0.0f;

                player->SetPos(spawnX, spawnY);

                // 3. 发送登录回包
                LoginRes res;
                res.set_ret_code(0);
                res.set_msg("Welcome to Aegis World!");
                res.set_entity_id(player->id().raw);
                auto *pos = res.mutable_pos();
                pos->set_x(spawnX);
                pos->set_y(spawnY);

                player->send_packet(ids::SC_LOGIN_RES, 0, res);

                // 4. 从 GameApp 获取默认主城 Scene
                ActorID scene_id = GameApp::instance().default_scene_id();
                auto *scene = ActorRegistry::instance().get(scene_id);

                if (scene)
                {
                    player->set_parent_id(scene_id);

                    auto *msg = new SceneEnterMsg(player->id(), uid, spawnX, spawnY);

                    if (scene->push(msg))
                    {
                        auto *target_worker = aegis::core::Scheduler::instance().get_worker(scene->worker_id());

                        if (aegis::core::Worker::get_current_id() == scene->worker_id())
                        {
                            target_worker->dispatch_local(scene);
                        }
                        else
                        {
                            target_worker->post_cross_core_task(scene);
                        }

                        Log::instance().info("[Login] Player {} dispatched to Scene {} on Worker {}",
                                             uid, scene_id.raw, scene->worker_id());
                    }
                }
                else
                {
                    Log::instance().error("[Login] CRITICAL: Main City Scene not found! ID: {}", scene_id.raw);
                }

                co_return;
            });

        // ==========================================================
        // Handler 2: C2S_CreateCampReq — 玩家请求创建营地
        // ==========================================================
        d.register_handler<C2S_CreateCampReq>(
            ids::C2S_CREATE_CAMP_REQ,
            [](aegis::core::Actor *actor, const C2S_CreateCampReq &req) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);

                // 1. 构建内部 RPC 请求 -> RoomManager
                AssignCampReq camp_req;
                camp_req.player_uid = player->id().raw;
                camp_req.player_actor_id = player->id();
                camp_req.camp_name = req.camp_name();
                camp_req.is_create = true;

                // 2. 发起异步 RPC 调用（非阻塞！）
                ActorID room_mgr_id = GameApp::instance().room_manager_id();
                auto *rpc_msg = new RPCAssignCampMsg(camp_req);
                rpc_msg->set_rpc_meta(0, player->id());

                // 使用 RpcCall 发送并等待
                AssignCampRes camp_res;
                try
                {
                    camp_res = co_await RpcCall<AssignCampRes>(room_mgr_id, rpc_msg);
                }
                catch (const std::runtime_error &e)
                {
                    Log::instance().error("[Camp][Create] RPC failed: {}", e.what());
                    co_return;
                }

                // 验证 Actor 仍然有效
                auto *current_player = static_cast<PlayerActor *>(
                    ActorRegistry::instance().get(player->id()));
                if (!current_player)
                {
                    Log::instance().warn("[Camp][Create] Player actor already destroyed");
                    co_return;
                }
                player = current_player;

                // 3. 构建回包
                S2C_CreateCampRes res;
                res.set_ret_code(camp_res.ret_code);
                res.set_scene_actor_id(camp_res.scene_actor_id.raw);
                res.set_camp_name(camp_res.camp_name);

                if (camp_res.ret_code == 0 && camp_res.scene_actor_id.is_valid())
                {
                    res.set_msg("Camp created successfully");

                    // 4. 让玩家离开当前场景
                    ActorID old_scene_id = player->parent_id();
                    auto *old_scene = ActorRegistry::instance().get(old_scene_id);
                    if (old_scene)
                    {
                        auto *leave_msg = new SceneLeaveMsg(player->id(), player->id().raw);
                        dispatch_msg(old_scene, leave_msg);
                    }

                    // 5. 进入新营地
                    player->set_parent_id(camp_res.scene_actor_id);
                    auto *new_scene = ActorRegistry::instance().get(camp_res.scene_actor_id);
                    if (new_scene)
                    {
                        auto *enter_msg = new SceneEnterMsg(
                            player->id(), player->id().raw, player->GetX(), player->GetY());
                        dispatch_msg(new_scene, enter_msg);
                    }

                    Log::instance().info("[Camp][Create] Player {} created camp '{}' (Scene {})",
                                         player->id().raw, camp_res.camp_name,
                                         camp_res.scene_actor_id.raw);
                }
                else
                {
                    res.set_msg(camp_res.err_msg.empty() ? "Camp creation failed" : camp_res.err_msg);
                    Log::instance().warn("[Camp][Create] Failed for player {}: {}",
                                         player->id().raw, camp_res.err_msg);
                }

                player->send_packet(ids::S2C_CREATE_CAMP_RES, 0, res);
                co_return;
            });

        // ==========================================================
        // Handler 3: C2S_JoinCampReq — 玩家请求加入营地
        // ==========================================================
        d.register_handler<C2S_JoinCampReq>(
            ids::C2S_JOIN_CAMP_REQ,
            [](aegis::core::Actor *actor, const C2S_JoinCampReq &req) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);

                // 1. 构建内部 RPC
                AssignCampReq camp_req;
                camp_req.player_uid = player->id().raw;
                camp_req.player_actor_id = player->id();
                camp_req.is_create = false;
                camp_req.target_scene_id = req.scene_actor_id();

                // 2. 发起异步 RPC
                ActorID room_mgr_id = GameApp::instance().room_manager_id();
                auto *rpc_msg = new RPCAssignCampMsg(camp_req);
                rpc_msg->set_rpc_meta(0, player->id());

                AssignCampRes camp_res;
                try
                {
                    camp_res = co_await RpcCall<AssignCampRes>(room_mgr_id, rpc_msg);
                }
                catch (const std::runtime_error &e)
                {
                    Log::instance().error("[Camp][Join] RPC failed: {}", e.what());
                    co_return;
                }

                // 验证 Actor
                auto *current_player = static_cast<PlayerActor *>(
                    ActorRegistry::instance().get(player->id()));
                if (!current_player)
                {
                    Log::instance().warn("[Camp][Join] Player actor already destroyed");
                    co_return;
                }
                player = current_player;

                // 3. 构建回包
                S2C_JoinCampRes res;
                res.set_ret_code(camp_res.ret_code);
                res.set_scene_actor_id(camp_res.scene_actor_id.raw);

                if (camp_res.ret_code == 0 && camp_res.scene_actor_id.is_valid())
                {
                    res.set_msg("Joined camp successfully");

                    // 4. 离开当前场景
                    ActorID old_scene_id = player->parent_id();
                    auto *old_scene = ActorRegistry::instance().get(old_scene_id);
                    if (old_scene)
                    {
                        auto *leave_msg = new SceneLeaveMsg(player->id(), player->id().raw);
                        dispatch_msg(old_scene, leave_msg);
                    }

                    // 5. 进入新营地
                    player->set_parent_id(camp_res.scene_actor_id);
                    auto *new_scene = ActorRegistry::instance().get(camp_res.scene_actor_id);
                    if (new_scene)
                    {
                        auto *enter_msg = new SceneEnterMsg(
                            player->id(), player->id().raw, player->GetX(), player->GetY());
                        dispatch_msg(new_scene, enter_msg);
                    }

                    Log::instance().info("[Camp][Join] Player {} joined camp '{}' (Scene {})",
                                         player->id().raw, camp_res.camp_name,
                                         camp_res.scene_actor_id.raw);
                }
                else
                {
                    res.set_msg(camp_res.err_msg.empty() ? "Camp not found" : camp_res.err_msg);
                    Log::instance().warn("[Camp][Join] Failed for player {}: {}",
                                         player->id().raw, camp_res.err_msg);
                }

                player->send_packet(ids::S2C_JOIN_CAMP_RES, 0, res);
                co_return;
            });

        // ==========================================================
        // Handler 4: Movement (转发给 Scene)
        // ==========================================================
        d.register_handler<CSMoveReq>(
            CS_MOVE_REQ,
            [](aegis::core::Actor *actor, const CSMoveReq &req) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);

                // [RECV] 移动请求日志
                Log::instance().info("[RECV][MoveReq] Player:{}, Pos:({:.2f},{:.2f})→({:.2f},{:.2f}), Speed:{:.2f}, Moving:{}, Scene:{}",
                                     player->id().raw,
                                     player->GetX(), player->GetY(),
                                     req.target_pos().x(), req.target_pos().y(),
                                     req.speed(), req.is_moving(),
                                     player->parent_id().raw);

                // 1. 获取父亲 (场景)
                ActorID scene_id = player->parent_id();
                auto *scene = ActorRegistry::instance().get(scene_id);

                if (scene)
                {
                    float newX = req.target_pos().x();
                    float newY = req.target_pos().y();
                    float speed = req.speed();
                    bool is_moving = req.is_moving();
                    float dir_angle = req.direction();

                    float oldX = player->GetX();
                    float oldY = player->GetY();
                    uint8_t dirty_flags = 0;
                    if (newX != oldX || newY != oldY)
                    {
                        dirty_flags |= PlayerActor::DIRTY_POS;
                    }

                    // 2. 更新移动状态（速度 / 移动标识 / 方向）
                    // 注意：不在这里调 SetPos！Actor 模型的铁律——空间状态变更只能
                    // 在 Actor 自己的线程上下文（SceneActor::OnHandleMove）中执行。
                    player->SetMoveState(speed, is_moving, dir_angle);

                    // 3. 转发给 SceneActor
                    auto *msg = new SceneMoveMsg(player->id(), player->id().raw, player->get_aoi_grid_index(), newX, newY, dirty_flags);
                    dispatch_msg(scene, msg);
                }
                else
                {
                    Log::instance().warn("[Move] Player {} has no valid scene supervisor.", player->id().raw);
                }

                co_return;
            });

        d.register_handler<CSUpdateStateReq>(
            ids::CS_UPDATE_STATE_REQ,
            [](aegis::core::Actor *actor, const CSUpdateStateReq &req) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);

                aegis::common::PlayerState new_state = req.state();

                player->SetState(new_state);

                Log::instance().debug("[State] Player {} state changed to {}", player->id().raw, (int)new_state);

                co_return;
            });

        // ==========================================================
        // Handler 6: Skill Cast
        // ==========================================================
        d.register_handler<CSSkillCastReq>(
            ids::CS_SKILL_CAST_REQ,
            [](aegis::core::Actor *actor, const CSSkillCastReq &req) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);
                ActorID scene_id = player->parent_id();
                auto *scene = ActorRegistry::instance().get(scene_id);

                if (scene)
                {
                    float tx = req.target_pos().x();
                    float ty = req.target_pos().y();

                    auto *msg = new SceneSkillCastMsg(player->id(), req.skill_id(), req.target_id(), tx, ty);
                    dispatch_msg(scene, msg);
                }
                co_return;
            });

        // ==========================================================
        // Handler 7: C2S_QueryCampListReq — 查询营地列表
        // ==========================================================
        d.register_handler<C2S_QueryCampListReq>(
            ids::C2S_QUERY_CAMP_LIST_REQ,
            [](aegis::core::Actor *actor, const C2S_QueryCampListReq & /*req*/) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);

                ActorID room_mgr_id = GameApp::instance().room_manager_id();
                auto *room_mgr = ActorRegistry::instance().get(room_mgr_id);
                if (!room_mgr)
                {
                    Log::instance().error("[QueryCamp] RoomManager not found!");
                    co_return;
                }

                // 直接读 RoomManager 的缓存（O(1)）
                auto *rm = static_cast<aegis::core::RoomManager *>(room_mgr);
                const auto &metas = rm->camp_metas();

                Log::instance().info("[QueryCamp] RoomManager has {} camps in metas",
                                     metas.size());

                S2C_QueryCampListRes res;
                int limit = 50; // 最多返回 50 个
                int count = 0;

                for (const auto &[id, meta] : metas)
                {
                    if (count >= limit)
                        break;

                    auto *info = res.add_camps();
                    info->set_scene_actor_id(meta.scene_actor_id);
                    info->set_camp_name(meta.camp_name);
                    info->set_current_players(meta.current_players);
                    info->set_max_players(meta.max_players);
                    count++;
                }

                res.set_total_count(static_cast<int>(metas.size()));

                // 构造回包（此时 PlayerActor 所在的 Worker 已从挂起恢复）
                Log::instance().info("[QueryCamp] Returning {} camps to player {}",
                                     count, player->id().raw);

                player->send_packet(ids::S2C_QUERY_CAMP_LIST_RES, 0, res);
                co_return;
            });

        Log::instance().info("[System] Logic Handlers Loaded.");

        // ==========================================================
        // Handler 8: C2S_CreateDungeonReq — 房主创建副本
        // ==========================================================
        d.register_handler<aegis::cs::dungeon::C2S_CreateDungeonReq>(
            ids::C2S_CREATE_DUNGEON_REQ,
            [](aegis::core::Actor *actor, const aegis::cs::dungeon::C2S_CreateDungeonReq &req) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);

                // 1. 构建 RPC 请求 -> RoomManager
                CreateDungeonReq dungeon_req;
                dungeon_req.player_uid = player->id().raw;
                dungeon_req.player_actor_id = player->id();
                dungeon_req.map_id = req.map_id();

                // 2. 发起 RPC 调用
                ActorID room_mgr_id = GameApp::instance().room_manager_id();
                auto *rpc_msg = new RPCCreateDungeonMsg(dungeon_req);
                rpc_msg->set_rpc_meta(0, player->id());

                CreateDungeonRes dungeon_res;
                try
                {
                    dungeon_res = co_await RpcCall<CreateDungeonRes>(room_mgr_id, rpc_msg);
                }
                catch (const std::runtime_error &e)
                {
                    Log::instance().error("[Dungeon][Create] RPC failed: {}", e.what());
                    co_return;
                }

                // 3. 验证 player 仍然存活
                auto *current_player = static_cast<PlayerActor *>(
                    ActorRegistry::instance().get(player->id()));
                if (!current_player)
                {
                    co_return;
                }
                player = current_player;

                // 4. 构建回包
                aegis::cs::dungeon::S2C_CreateDungeonRes res;
                res.set_ret_code(dungeon_res.ret_code);
                res.set_msg(dungeon_res.err_msg);
                res.set_dungeon_scene_id(dungeon_res.dungeon_scene_id.raw);

                if (dungeon_res.ret_code == 0 && dungeon_res.dungeon_scene_id.is_valid())
                {
                    // 5. 让玩家离开当前场景
                    ActorID old_scene_id = player->parent_id();
                    auto *old_scene = ActorRegistry::instance().get(old_scene_id);
                    if (old_scene)
                    {
                        auto *leave_msg = new SceneLeaveMsg(player->id(), player->id().raw);
                        dispatch_msg(old_scene, leave_msg);
                    }

                    // 6. 进入副本场景
                    player->set_parent_id(dungeon_res.dungeon_scene_id);
                    auto *new_scene = ActorRegistry::instance().get(dungeon_res.dungeon_scene_id);
                    if (new_scene)
                    {
                        auto *enter_msg = new SceneEnterMsg(
                            player->id(), player->id().raw, player->GetX(), player->GetY());
                        dispatch_msg(new_scene, enter_msg);
                    }

                    Log::instance().info("[Dungeon][Create] Player {} created dungeon (Scene {})",
                                         player->id().raw, dungeon_res.dungeon_scene_id.raw);
                }
                else
                {
                    res.set_msg(dungeon_res.err_msg.empty() ? "Dungeon creation failed" : dungeon_res.err_msg);
                }

                player->send_packet(ids::S2C_CREATE_DUNGEON_RES, 0, res);
                co_return;
            });
    }
}
