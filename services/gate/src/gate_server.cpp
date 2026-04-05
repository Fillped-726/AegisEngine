#include "gate_server.h"

#include <iostream>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <stdexcept>
#include <sys/resource.h>

// Core Framework
#include "aegis/core/worker.h"
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include "aegis/core/scheduler.h"
#include "aegis/net/socket.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/hierarchy_timer.h"
#include "aegis/net/packetPool.h"
#include "handler_loader.h"
#include "aegis/net/acceptor.h"

// Business Logic
#include "aegis/core/playerActor.h"
#include "aegis/core/npc_actor.h"
#include "aegis/core/scene_actor.h" // [New]
#include "ids.pb.h"
#include "cs_battle.pb.h" // [New]
#include "aegis/core/room_manager.h"
#include "aegis/common/tools.h"

using namespace aegis;

namespace aegis::core
{
    extern thread_local core::Worker *t_current_worker;
}

namespace aegis::gate
{

    void tune_fd_limit()
    {
        struct rlimit rl;
        // 获取当前限制
        if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
        {
            perror("getrlimit");
            return;
        }

        // 将软限制提升至硬限制的水平（即 1048576）
        rl.rlim_cur = rl.rlim_max;

        if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
        {
            perror("setrlimit"); // 如果失败，通常是因为尝试超过硬限制
        }
        else
        {
            std::cout << "Successfully raised FD limit to: " << rl.rlim_cur << std::endl;
        }
    }

    GateServer::~GateServer()
    {
        stop();
    }

    void GateServer::init(const std::string &config_path, int num_workers)
    {
        tune_fd_limit();

        // 1. 初始化日志
        Log::instance().init_config(config_path, "Gate");
        Log::instance().set_level(spdlog::level::debug);

        // 1. 【Bootstrap】创建全局 RoomManager
        room_manager_id_ = core::ActorRegistry::instance().create_actor<core::RoomManager>();
        Log::instance().info("[Init] RoomManager Created. ID: {}", room_manager_id_.raw);

        // 2. 直接同步创建默认场景 (主城)
        auto scene_id = core::ActorRegistry::instance().create_actor<core::SceneActor>(500.0f, 500.0f, 10.0f);

        auto *scene = core::ActorRegistry::instance().get(scene_id);
        if (scene)
        {
            scene->set_worker_id(3);
        }

        if (scene_id.is_valid())
        {
            // A. 保存到全局变量，供 Handler 使用
            g_DefaultSceneID = scene_id;

            // B. 手动认父 (Supervision)
            // 让 RoomManager 成为它的监管者，负责崩溃重启
            auto *scene = core::ActorRegistry::instance().get(scene_id);
            if (scene)
            {
                scene->set_parent_id(room_manager_id_);

                // 【新增：强转为 SceneActor 以便调用 AddNpc】
                auto *concrete_scene = static_cast<core::SceneActor *>(scene);

                // 【新增：在场景中央 (250, 250) 附近刷 3 个测试 NPC】
                for (int i = 0; i < 3; ++i)
                {
                    auto npc_id = core::ActorRegistry::instance().create_actor<core::NpcActor>(); // 注意之前改的带参构造
                    if (npc_id.is_valid())
                    {
                        auto *npc = static_cast<core::NpcActor *>(core::ActorRegistry::instance().get(npc_id));
                        npc->reset(npc_id, 250.0f + i * 5.0f, 250.0f + i * 5.0f);
                        concrete_scene->AddNpc(npc);
                        Log::instance().info("[Init] Spawned Test NPC {} at ({}, {})", npc_id.raw, npc->GetX(), npc->GetY());
                    }
                }
                Log::instance().info("[Init] Main City (Scene) Created. ID: {}", scene_id.raw);
            }

            Log::instance().info("[Init] Main City (Scene) Created. ID: {}", scene_id.raw);
        }
        else
        {
            Log::instance().critical("[Init] Failed to create Main City!");
            throw std::runtime_error("Scene creation failed");
        }

        // 3. 注入业务逻辑 [Critical Step]
        // 将 scene_ 传递给逻辑层，实现了 GateServer 与 具体逻辑 的解耦
        load_handlers();

        // 4. 启动调度器
        if (num_workers <= 0)
        {
            num_workers = std::thread::hardware_concurrency();
            if (num_workers < 1)
                num_workers = 1;
        }
        Log::instance().info("[Init] Starting Scheduler with {} workers...", num_workers);
        core::Scheduler::instance().start(num_workers);
    }

    void GateServer::run(int port)
    {
        try
        {
            Log::instance().info("[Gate] Dispatching Acceptor to Worker 0...");

            auto *worker0 = core::Scheduler::instance().get_worker(0);
            worker0->post_custom_task([this, port]()
                                      { this->accept_loop(port); });

            // 主线程化身为守护者，仅仅阻塞防止程序退出
            Log::instance().info("[Gate] Main thread entering wait state.");
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        catch (const std::exception &e)
        {
            Log::instance().critical("Exception in GateServer::run: {}", e.what());
            throw;
        }
    }

    void GateServer::stop()
    {
        Log::instance().info("[Shutdown] Stopping Scheduler...");
        core::Scheduler::instance().stop();
    }

    core::DetachedTask GateServer::handle_session(net::Socket client_socket)
    {
        // 【此时此刻的震撼】：
        // 这行代码执行时，我们已经身处 target_worker_id 对应的线程里了！
        // 接下来所有的 IO、Actor 计算，全部在这个 L1 Cache 极度亲和的核心里打转！

        auto client_fd = client_socket.native_handle();
        auto conn = std::make_shared<net::Connection>(std::move(client_socket));

        core::ActorID player_id = core::ActorRegistry::instance().create_actor<core::PlayerActor>(conn);

        if (!player_id.is_valid())
        {
            Log::instance().error("Failed to create actor for fd: {}", client_fd);
            co_return;
        }

        try
        {
            while (true)
            {
                // 这里的 co_await 会毫无阻碍地使用当前 Worker 的 io_uring
                auto packet = co_await conn->read_packet();
                if (!packet)
                    break;

                auto *target_actor = core::ActorRegistry::instance().get(player_id);
                if (target_actor)
                {
                    auto msg = core::NetworkMessagePool::instance().acquire(std::move(packet), client_fd);

                    dispatch_to_actor(target_actor, msg.release());
                }
                else
                {
                    break;
                }
            }
        }
        catch (const std::exception &e)
        {
            Log::instance().error("[Gate] Error: {}", e.what());
        }

        // 断开处理
        auto *target_actor = core::ActorRegistry::instance().get(player_id);
        if (target_actor)
        {
            dispatch_to_actor(target_actor, new core::SessionClosedMsg(0));
        }

        Log::instance().info("[Gate] Connection Closed: {}", client_fd);
    }

    // 监听协程 (基本未变，只是日志移到了 Log 库)
    core::DetachedTask GateServer::accept_loop(int port)
    {
        try
        {
            net::Acceptor acceptor(port);
            uint64_t connection_counter = 0;

            Log::instance().info("[Acceptor] Start listening on port {} (Running on Worker 0)", port);

            while (true)
            {
                net::Socket client_socket = co_await acceptor.accept();

                if (client_socket.is_valid())
                {
                    // 【核心巨变：跨核 Socket 移交】
                    int total_workers = 4;
                    int target_worker_id = (connection_counter++) % total_workers;

                    auto *target_worker = core::Scheduler::instance().get_worker(target_worker_id);

                    // 把 Socket 转移(move)给目标 Worker，让他在自己的核上拉起协程！
                    target_worker->post_custom_task([this, s = std::move(client_socket)]() mutable
                                                    { this->handle_session(std::move(s)); });
                }
                else if (errno == EMFILE || errno == ENFILE)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
        catch (const std::exception &e)
        {
            Log::instance().critical("Accept Loop Fatal Error: {}", e.what());
        }
    }

    void GateServer::dispatch_to_actor(core::Actor *actor, core::ActorMessage *msg)
    {
        if (!actor || !msg)
            return;

        if (actor->push(msg))
        {
            if (actor->worker_id() == core::t_current_worker->id())
            {
                core::t_current_worker->dispatch_local(actor);
            }
            else
            {
                core::Scheduler::instance().get_worker(actor->worker_id())->post_cross_core_task(actor);
            }
        }
    }

} // namespace aegis::gate