#include "handler_loader.h"

// 引入必要的库
#include "gate_server.h" // [重要] 需要访问 GateServer::g_DefaultSceneID
#include "aegis/net/dispatcher.h"
#include "aegis/core/playerActor.h"
#include "aegis/core/scene_actor.h"
#include "aegis/core/scheduler.h"
#include "aegis/core/actor_registry.h" // [重要] 引入 Registry
#include "aegis/common/aegisLog.h"
#include "aegis/core/message.h" // 引入刚才定义的消息
#include "aegis/core/GameMessage.h"
#include "aegis/core/worker.h"
#include "aegis/common/actor_utils.h" // 引入 dispatch_msg 函数

// 引入 Protocol Buffers
#include "cs_lobby.pb.h"
#include "cs_battle.pb.h"
#include "ids.pb.h"

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
            // [建议] 使用 ids.pb.h 里的枚举，不要硬编码数字，防止对不上
            ids::CS_LOGIN_REQ,
            [](aegis::core::Actor *actor, const LoginReq &req) -> aegis::core::Task<void>
            {
                // 使用 dynamic_cast 更安全，防止非法 Actor 调用
                auto player = dynamic_cast<aegis::core::PlayerActor *>(actor);
                if (!player) [[unlikely]]
                {
                    Log::instance().error("[Logic] Handler called with invalid actor type");
                    co_return;
                }

                uint64_t uid = req.uid();

                // 1. [核心修复] 必须给 PlayerActor 设置业务 ID (UID)
                // 之前就是因为缺了这行，导致 PlayerActor 里的 playerId_ 是 0
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

                // 4. 获取主城 Scene
                ActorID scene_id = GateServer::g_DefaultSceneID;
                auto *scene = ActorRegistry::instance().get(scene_id);

                if (scene)
                {

                    player->set_parent_id(scene_id);

                    auto *msg = new SceneEnterMsg(player->id(), uid, spawnX, spawnY);

                    // 压入消息队列
                    if (scene->push(msg))
                    {
                        // [核心修复]：拒绝 Round-Robin，定向投递回它出生的 Worker
                        auto *target_worker = aegis::core::Scheduler::instance().get_worker(scene->worker_id());

                        // 如果当前就在这个 Worker，直接进快轨；否则走跨核队列
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
                    // 实际生产中这里应该断开客户端连接或者返回错误码
                }

                co_return;
            });

        // ==========================================================
        // Handler 2: Movement (转发给 Scene)
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

                    // 3. [修复] 构造正确的参数顺序: id, uid, oldGridIndex, newX, newY
                    auto *msg = new SceneMoveMsg(player->id(), player->get_player_id(), player->get_aoi_grid_index(), newX, newY, direction);

                    dispatch_msg(scene, msg);
                }
                else
                {
                    // 日志需要访问 .raw
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
        // Handler 4: Skill Cast (客户端请求释放技能)
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
                    // 提取目标位置
                    float tx = req.target_pos().x();
                    float ty = req.target_pos().y();

                    // 打包投递给 SceneActor 的无锁 Tick 队列
                    auto *msg = new SceneSkillCastMsg(player->id(), req.skill_id(), req.target_id(), tx, ty);
                    dispatch_msg(scene, msg);
                }
                co_return;
            });

        Log::instance().info("[System] Logic Handlers Loaded.");
    }
}