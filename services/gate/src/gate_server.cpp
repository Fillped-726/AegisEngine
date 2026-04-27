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

// Core Framework (only)
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

// PlayerActor only — the gateway needs to create player sessions
#include "aegis/game/playerActor.h"

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
        if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
        {
            perror("getrlimit");
            return;
        }

        rl.rlim_cur = rl.rlim_max;

        if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
        {
            perror("setrlimit");
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

        // 2. 注入业务逻辑处理器
        load_handlers();

        // 3. 启动调度器
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
                    int total_workers = 4;
                    int target_worker_id = (connection_counter++) % total_workers;

                    auto *target_worker = core::Scheduler::instance().get_worker(target_worker_id);

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
