#include "gate_server.h"

#include <iostream>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
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

// Business Logic
#include "aegis/core/playerActor.h"
#include "aegis/core/scene_actor.h" // [New]
#include "common.pb.h"
#include "scene.pb.h" // [New]

using namespace aegis;

namespace aegis::gate
{
    // [Config] 协议常量定义在匿名命名空间，避免污染全局
    namespace
    {
        constexpr uint32_t MSG_CS_LOGIN_REQ = 1001;
        constexpr uint32_t MSG_SC_LOGIN_RES = 1002;
        constexpr uint32_t MSG_CS_MOVE_REQ = 2001;
    }

    GateServer::~GateServer()
    {
        stop();
    }

    void GateServer::init(const std::string &config_path, int num_workers)
    {
        // 1. 初始化日志
        Log::instance().init_config(config_path, "Gate");
        Log::instance().set_level(spdlog::level::warn);

        // 2. 初始化核心资源 (场景)
        init_scene();

        // 3. 注入业务逻辑 [Critical Step]
        // 将 scene_ 传递给逻辑层，实现了 GateServer 与 具体逻辑 的解耦
        load_handlers(scene_);

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

    // =========================================================
    // Private Methods
    // =========================================================

    void GateServer::init_scene()
    {
        // [New] 创建场景 500x500
        scene_ = std::make_shared<core::SceneActor>(500.0f, 500.0f, 10.0f);
        Log::instance().info("[Init] Global Scene Created (500x500).");
    }

    core::DetachedTask GateServer::handle_session(int client_fd)
    {
        auto conn = std::make_shared<net::Connection>(net::Socket(client_fd));

        // 使用 FD 作为临时 ID，实际逻辑中 Login 成功后应更新为 DB UID
        auto actor = new core::PlayerActor(conn, static_cast<uint64_t>(client_fd));

        try
        {
            while (true)
            {
                auto packet = co_await conn->read_packet();
                if (!packet)
                    break;

                auto msg = core::NetworkMessagePool::instance().acquire(std::move(packet), client_fd);
                // 投递到 PlayerActor，调度器会在 Worker 线程执行 init_handlers 里注册的逻辑
                if (actor->push(msg.release()))
                {
                    core::Scheduler::instance().dispatch(actor);
                }
            }
        }
        catch (const std::exception &e)
        {
            Log::instance().error("[Gate] Session Error: {}", e.what());
        }

        // 1. 通知 Scene 玩家离开
        if (scene_)
        {
            auto *msg = new core::SceneLeaveMsg(actor->GetID());
            if (scene_->push(msg))
            {
                core::Scheduler::instance().dispatch(scene_.get());
            }
        }

        // 2. 销毁 PlayerActor
        auto *close_msg = new core::SessionClosedMsg(0);
        if (actor->push(close_msg))
        {
            core::Scheduler::instance().dispatch(actor);
        }

        Log::instance().info("[Gate] Session Lifecycle Ended | FD: {}", client_fd);
    }

    // 监听协程 (基本未变，只是日志移到了 Log 库)
    core::DetachedTask GateServer::accept_loop(int port)
    {
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0)
            throw std::runtime_error("socket failed");

        net::Socket listener(listen_fd);
        int val = 1;
        setsockopt(listener.native_handle(), SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listener.native_handle(), (sockaddr *)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (listen(listener.native_handle(), 128) < 0)
            throw std::runtime_error("listen failed");

        Log::instance().info(">>> Aegis Game Server Listening on Port {} <<<", port);

        while (true)
        {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = co_await listener.accept((sockaddr *)&client_addr, &len);

            if (client_fd >= 0)
            {
                handle_session(client_fd);
            }
            else if (errno == EMFILE || errno == ENFILE)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
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