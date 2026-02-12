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
                float spawnX = 100.0f + (uid % 10);
                float spawnY = 100.0f + (uid % 10);

                player->SetPos(spawnX, spawnY);

                // 3. 发送登录回包
                LoginRes res;
                res.set_ret_code(0);
                res.set_msg("Welcome to Aegis World!");
                // [建议] 确保 SC_LOGIN_RES 对应 ids::SC_LOGIN_RES (1002)
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
                        // 唤醒 Scene Actor
                        aegis::core::Scheduler::instance().dispatch(scene);
                        Log::instance().info("[Login] Player {} dispatched to Scene {}", uid, scene_id.raw);
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

                    // [修复] 必须先保存旧位置，因为 SceneMoveMsg 需要它
                    float oldX = player->GetX();
                    float oldY = player->GetY();

                    // 2. 更新玩家自身数据 (乐观更新)
                    player->SetPos(newX, newY);

                    // 3. [修复] 构造正确的参数顺序: id, uid, oldX, oldY, newX, newY
                    auto *msg = new SceneMoveMsg(player->id(), player->get_player_id(), oldX, oldY, newX, newY);

                    if (scene->push(msg))
                    {
                        aegis::core::Scheduler::instance().dispatch(scene);
                    }
                }
                else
                {
                    // 日志需要访问 .raw
                    Log::instance().warn("[Move] Player {} has no valid scene supervisor.", player->id().raw);
                }

                co_return;
            });

        Log::instance().info("[System] Logic Handlers Loaded.");
    }
}