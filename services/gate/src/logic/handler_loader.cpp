#include "handler_loader.h"

// 引入必要的库
#include "aegis/net/dispatcher.h"
#include "aegis/core/playerActor.h"
#include "aegis/core/scene_actor.h"
#include "aegis/core/scheduler.h"
#include "aegis/common/aegisLog.h"

// 引入 Protocol Buffers
#include "common.pb.h"
#include "scene.pb.h"

// 协议 ID (实际项目中建议放在 common/protocol_ids.h)
namespace
{
    constexpr uint32_t MSG_CS_LOGIN_REQ = 1001;
    constexpr uint32_t MSG_SC_LOGIN_RES = 1002;
    constexpr uint32_t MSG_CS_MOVE_REQ = 2001;
}

namespace aegis::gate
{
    void load_handlers(std::shared_ptr<aegis::core::SceneActor> global_scene)
    {
        auto &d = aegis::net::Dispatcher::instance();

        // ==========================================================
        // Handler 1: Login
        // ==========================================================
        d.register_handler<protocol::LoginReq>(
            MSG_CS_LOGIN_REQ,
            [scene = global_scene](aegis::core::Actor *actor, const protocol::LoginReq &req) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);

                // 打印日志 (注意：LogFormat 需要你之前的 Log 库支持)
                Log::instance().info("[Logic] Login Request | UID: {}", req.uid());

                // 1. 简单的出生点计算逻辑
                float spawnX = 100.0f + (req.uid() % 10);
                float spawnY = 100.0f + (req.uid() % 10);
                player->SetPos(spawnX, spawnY);

                // 2. 发送回包
                protocol::LoginRes res;
                res.set_ret_code(0);
                res.set_msg("Welcome to Aegis World!");
                player->send_packet(MSG_SC_LOGIN_RES, res);

                // 3. 通知场景有人进入 (解耦关键：使用捕获的 scene 变量，而不是 this)
                if (scene)
                {
                    auto *msg = new aegis::core::SceneEnterMsg(player, spawnX, spawnY);
                    if (scene->push(msg))
                    {
                        aegis::core::Scheduler::instance().dispatch(scene.get());
                    }
                }
                co_return;
            });

        // ==========================================================
        // Handler 2: Movement
        // ==========================================================
        d.register_handler<protocol::CSMoveReq>(
            MSG_CS_MOVE_REQ,
            [scene = global_scene](aegis::core::Actor *actor, const protocol::CSMoveReq &req) -> aegis::core::Task<void>
            {
                auto player = static_cast<aegis::core::PlayerActor *>(actor);

                float oldX = player->GetX();
                float oldY = player->GetY();
                float newX = req.target_pos().x();
                float newY = req.target_pos().y();

                // 更新 Actor 自身数据
                player->SetPos(newX, newY);

                // 通知场景进行广播
                if (scene)
                {
                    auto *msg = new aegis::core::SceneMoveMsg(player->GetID(), oldX, oldY, newX, newY);
                    if (scene->push(msg))
                    {
                        aegis::core::Scheduler::instance().dispatch(scene.get());
                    }
                }
                co_return;
            });

        Log::instance().info("[System] Logic Handlers Loaded.");
    }
}