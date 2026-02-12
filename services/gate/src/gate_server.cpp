#include "gate_server.h"

#include <iostream>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <stdexcept>

// Core Framework
#include "aegis/core/env.h"
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
#include "aegis/core/scene_actor.h" // [New]
#include "ids.pb.h"
#include "cs_battle.pb.h" // [New]
#include "aegis/core/room_manager.h"

using namespace aegis;

namespace aegis::gate
{

    GateServer::~GateServer()
    {
        stop();
    }

    void GateServer::init(const std::string &config_path, int num_workers)
    {
        // 1. 初始化日志
        Log::instance().init_config(config_path, "Gate");
        Log::instance().set_level(spdlog::level::warn);

        // 1. 【Bootstrap】创建全局 RoomManager
        // 既然是直连，我们在这里手动启动"上帝 Actor"
        room_manager_id_ = core::ActorRegistry::instance().create_actor<core::RoomManager>();
        Log::instance().info("[Init] RoomManager Created. ID: {}", room_manager_id_.raw);

        // 2. [修改] 直接同步创建默认场景 (主城)
        // 不再使用 Fake RPC，确保 init 完成时场景一定存在
        auto scene_id = core::ActorRegistry::instance().create_actor<core::SceneActor>(500.0f, 500.0f, 10.0f);

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

        // 5. 初始化 IO 环境
        core::Env::instance().init();
    }

    void GateServer::run(int port)
    {
        try
        {
            start_timer();
            accept_loop(port);

            Log::instance().info("[Init] Entering Main IO Loop.");
            core::Env::instance().run();
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
        // 1. 物理连接封装
        auto conn = std::make_shared<net::Connection>(std::move(client_socket));

        // 2. 【核心】通过 Registry 创建 PlayerActor
        // 此时玩家还是"游离态"，没有进入任何房间
        core::ActorID player_id = core::ActorRegistry::instance()
                                      .create_actor<core::PlayerActor>(conn);

        if (!player_id.is_valid())
        {
            Log::instance().error("Failed to create actor for fd: {}", client_socket.native_handle());
            co_return;
        }

        // 本地会话记录
        ClientSession session;
        session.actor_id = player_id;

        try
        {
            while (true)
            {
                auto packet = co_await conn->read_packet();
                if (!packet)
                    break; // 连接断开

                // 3. 【核心】路由消息
                auto *actor = core::ActorRegistry::instance().get(player_id);
                if (actor)
                {
                    // 封装成 NetworkMessage
                    auto msg = core::NetworkMessagePool::instance().acquire(std::move(packet), client_socket.native_handle());

                    Log::instance().debug("[Trace] 1. NetMsg Created. Ptr: {}, TypeID: {} (Expect: 1)",
                                          (void *)msg.get(), (int)msg->type_id);

                    // 投递并调度
                    if (actor->push(msg.release()))
                    {
                        core::Scheduler::instance().dispatch(actor);
                    }
                }
                else
                {
                    // Actor 可能已经被踢下线或销毁
                    break;
                }
            }
        }
        catch (const std::exception &e)
        {
            Log::instance().error("[Gate] Error: {}", e.what());
        }

        // 4. 【核心】断开处理
        // Gate 不负责销毁 Actor，只通知它"网线拔了"
        auto *actor = core::ActorRegistry::instance().get(player_id);
        if (actor)
        {
            // PlayerActor 收到这个消息后，应该触发存盘、退出场景等逻辑，最后 finalize()
            auto *msg = new core::SessionClosedMsg(0);
            if (actor->push(msg))
            {
                core::Scheduler::instance().dispatch(actor);
            }
        }

        Log::instance().info("[Gate] Connection Closed: {}", client_socket.native_handle());
    }

    // 监听协程 (基本未变，只是日志移到了 Log 库)
    core::DetachedTask GateServer::accept_loop(int port)
    {
        try
        {
            // 1. 创建 Acceptor (RAII 管理)
            net::Acceptor acceptor(port);

            while (true)
            {
                // 2. 异步等待新连接 (返回封装好的 Socket 对象)
                net::Socket client_socket = co_await acceptor.accept();

                if (client_socket.is_valid())
                {
                    // 3. 处理会话 (注意 handle_session 参数需要改一下，或者取 native_handle)
                    // 建议 handle_session 直接接收 Socket 对象，或者传 fd
                    handle_session(std::move(client_socket));
                    // release() 释放所有权给 handle_session，防止析构关闭 fd
                }
                else
                {
                    // 处理 EMFILE 等临时错误
                    if (errno == EMFILE || errno == ENFILE)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            Log::instance().critical("Accept Loop Fatal Error: {}", e.what());
            // 决定是退出还是重启 loop
        }
    }

    // Timer Loop
    void GateServer::start_timer() { timer_loop(); }

    core::DetachedTask GateServer::timer_loop()
    {
        auto &wheel = core::HierarchicalTimeWheel::instance();
        wheel.init();
        int fd = wheel.get_fd();
        if (fd < 0)
            co_return;

        while (true)
        {
            uint64_t expirations = 0;
            int n = co_await net::Socket::AsyncRead(fd, &expirations, sizeof(expirations));
            if (n != 8)
                break;

            for (uint64_t i = 0; i < expirations; ++i)
                wheel.tick();
        }
    }

} // namespace aegis::gate