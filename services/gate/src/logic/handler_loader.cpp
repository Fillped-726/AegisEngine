#include "handler_loader.h"

// Core framework
#include "aegis/net/dispatcher.h"
#include "aegis/core/playerActor.h"
#include "aegis/core/scheduler.h"
#include "aegis/core/actor_registry.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/message/message.h"
#include "aegis/core/worker.h"
#include "aegis/common/actor_utils.h"

// Protocol Buffers
#include "cs_lobby.pb.h"
#include "cs_battle.pb.h"
#include "ids.pb.h"

// GameApp — centralized business bootstrap
#include "aegis/core/game_app.h"

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

                // 1. 设置 PlayerActor 的业务 ID
                player->set_player_id(uid);

                Log::instance().info("[Logic] Login Request | UID: {} -> ActorID: {}", uid, player->id().raw);

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

                player->send_packet(ids::SC_LOGIN_RES, res);

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
                camp_req.player_uid = player->get_player_id();
                camp_req.player_actor_id = player->id();
                camp_req.camp_name = req.camp_name();
                camp_req.is_create = true;

                // 2. 发送 RPC 给 RoomManager
                ActorID room_mgr_id = GameApp::instance().room_manager_id();
                auto *room_mgr = ActorRegistry::instance().get(room_mgr_id);

                if (!room_mgr)
                {
                    Log::instance().error("[Camp][Create] RoomManager not found!");
                    co_return;
                }

                auto *rpc_msg = new RPCAssignCampMsg(camp_req);
                auto future = rpc_msg->promise.get_future();
                dispatch_msg(room_mgr, rpc_msg);

                // 3. 等待 RoomManager 处理结果 (快速本地 RPC)
                // 注意: 这会阻塞当前协程的线程，但 RoomManager 在其 Worker 上立即处理
                AssignCampRes camp_res = future.get();

                // 4. 构建回包
                S2C_CreateCampRes res;
                res.set_ret_code(camp_res.ret_code);
                res.set_scene_actor_id(camp_res.scene_actor_id.raw);
                res.set_camp_name(camp_res.camp_name);

                if (camp_res.ret_code == 0 && camp_res.scene_actor_id.is_valid())
                {
                    res.set_msg("Camp created successfully");

                    // 5. 让玩家离开当前场景（默认主城），进入新营地
                    ActorID old_scene_id = player->parent_id();
                    auto *old_scene = ActorRegistry::instance().get(old_scene_id);
                    if (old_scene)
                    {
                        auto *leave_msg = new SceneLeaveMsg(player->id(), player->get_player_id());
                        dispatch_msg(old_scene, leave_msg);
                    }

                    // 6. 进入新营地
                    player->set_parent_id(camp_res.scene_actor_id);
                    auto *new_scene = ActorRegistry::instance().get(camp_res.scene_actor_id);
                    if (new_scene)
                    {
                        auto *enter_msg = new SceneEnterMsg(
                            player->id(), player->get_player_id(), player->GetX(), player->GetY());
                        dispatch_msg(new_scene, enter_msg);
                    }

                    Log::instance().info("[Camp][Create] Player {} created camp '{}' (Scene {})",
                                         player->get_player_id(), camp_res.camp_name,
                                         camp_res.scene_actor_id.raw);
                }
                else
                {
                    res.set_msg(camp_res.err_msg.empty() ? "Camp creation failed" : camp_res.err_msg);
                    Log::instance().warn("[Camp][Create] Failed for player {}: {}",
                                         player->get_player_id(), camp_res.err_msg);
                }

                player->send_packet(ids::S2C_CREATE_CAMP_RES, res);
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
                camp_req.player_uid = player->get_player_id();
                camp_req.player_actor_id = player->id();
                camp_req.is_create = false;
                camp_req.target_scene_id = req.scene_actor_id();

                // 2. 发送给 RoomManager
                ActorID room_mgr_id = GameApp::instance().room_manager_id();
                auto *room_mgr = ActorRegistry::instance().get(room_mgr_id);

                if (!room_mgr)
                {
                    Log::instance().error("[Camp][Join] RoomManager not found!");
                    co_return;
                }

                auto *rpc_msg = new RPCAssignCampMsg(camp_req);
                auto future = rpc_msg->promise.get_future();
                dispatch_msg(room_mgr, rpc_msg);

                // 3. 等待结果
                AssignCampRes camp_res = future.get();

                // 4. 构建回包
                S2C_JoinCampRes res;
                res.set_ret_code(camp_res.ret_code);
                res.set_scene_actor_id(camp_res.scene_actor_id.raw);

                if (camp_res.ret_code == 0 && camp_res.scene_actor_id.is_valid())
                {
                    res.set_msg("Joined camp successfully");

                    // 5. 离开当前场景
                    ActorID old_scene_id = player->parent_id();
                    auto *old_scene = ActorRegistry::instance().get(old_scene_id);
                    if (old_scene)
                    {
                        auto *leave_msg = new SceneLeaveMsg(player->id(), player->get_player_id());
                        dispatch_msg(old_scene, leave_msg);
                    }

                    // 6. 进入新营地
                    player->set_parent_id(camp_res.scene_actor_id);
                    auto *new_scene = ActorRegistry::instance().get(camp_res.scene_actor_id);
                    if (new_scene)
                    {
                        auto *enter_msg = new SceneEnterMsg(
                            player->id(), player->get_player_id(), player->GetX(), player->GetY());
                        dispatch_msg(new_scene, enter_msg);
                    }

                    Log::instance().info("[Camp][Join] Player {} joined camp '{}' (Scene {})",
                                         player->get_player_id(), camp_res.camp_name,
                                         camp_res.scene_actor_id.raw);
                }
                else
                {
                    res.set_msg(camp_res.err_msg.empty() ? "Camp not found" : camp_res.err_msg);
                    Log::instance().warn("[Camp][Join] Failed for player {}: {}",
                                         player->get_player_id(), camp_res.err_msg);
                }

                player->send_packet(ids::S2C_JOIN_CAMP_RES, res);
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

                // 1. 获取父亲 (场景)
                ActorID scene_id = player->parent_id();
                auto *scene = ActorRegistry::instance().get(scene_id);

                if (scene)
                {
                    float newX = req.target_pos().x();
                    float newY = req.target_pos().y();

                    float oldX = player->GetX();
                    float oldY = player->GetY();
                    unsigned direction = 0;
                    if (newX != oldX || newY != oldY)
                    {
                        direction |= PlayerActor::DIRTY_POS;
                    }

                    // 2. 更新玩家自身数据 (乐观更新)
                    player->SetPos(newX, newY);

                    // 3. 转发给 SceneActor
                    auto *msg = new SceneMoveMsg(player->id(), player->get_player_id(), player->get_aoi_grid_index(), newX, newY, direction);
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

        Log::instance().info("[System] Logic Handlers Loaded.");
    }
}
