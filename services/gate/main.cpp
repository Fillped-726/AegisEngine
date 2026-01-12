#include <iostream>
#include <vector>
#include <thread>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>

// Core Framework
#include "aegis/core/env.h"
#include "aegis/core/task.h"
// Macro cleanup if needed
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include "aegis/core/scheduler.h"
#include "aegis/net/socket.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"

// Business Logic
#include "playerActor.h"
#include "common.pb.h"

using namespace aegis;

// --- 业务协程 (Business Coroutines) ---

/**
 * @brief 处理单个客户端连接的生命周期 (Per-Session Coroutine)
 * @note 这是一个 "Fire-and-Forget" 协程 (DetachedTask)。
 */
core::DetachedTask handle_client(int client_fd)
{
    // [RAII] Connection 托管 socket FD，确保异常安全
    auto conn = std::make_shared<net::Connection>(net::Socket(client_fd));

    // [Optimization] 禁用 Nagle 算法 (TCP_NODELAY)
    // conn->socket().set_nodelay(true);

    aegis::Log::instance().info("[Gate] New Session Accepted | FD: {}", client_fd);

    // [Architecture] 创建 Actor
    // 此时 Actor 处于 "Idle" 状态，等待第一条消息激活
    auto actor = std::make_shared<gate::PlayerActor>(conn);

    try
    {
        while (true)
        {
            // [Async IO] 挂起等待数据。此时 CPU 让出给 IO Loop 处理其他事件。
            auto packet = co_await conn->read_packet();

            // [EOF Check] 对端关闭或错误
            if (!packet)
            {
                aegis::Log::instance().info("[Gate] Peer Closed Connection | FD: {}", client_fd);
                break;
            }

            // [Trace] 开发调试日志
            // aegis::Log::instance().debug("[Gate] Recv Packet | FD: {} | Size: {}B", client_fd, packet->payload_.size());

            // [Allocation] 封装为内部消息
            // 使用 std::move 转移 payload 内存，零拷贝
            auto *msg = new core::NetworkMessage(std::move(*packet), client_fd);

            // [Dispatch] 投递到 Actor 邮箱
            // actor->push 是无锁的。如果 Actor 之前是 Idle，则将其放入 Scheduler 待执行队列。
            if (actor->push(msg))
            {
                core::Scheduler::instance().dispatch(actor);
            }
        }
    }
    catch (const std::exception &e)
    {
        aegis::Log::instance().error("[Gate] Unhandled Exception in Session | FD: {} | What: {}", client_fd, e.what());
    }

    // --- Cleanup Phase ---

    // 发送断开通知给 Actor (用于清理逻辑数据，如存盘)
    auto *close_msg = new gate::SessionClosedMsg(0);

    // 即使连接断了，Actor 对象还在内存中 (shared_ptr)，
    // 需要最后一次激活它来处理 SessionClosedMsg
    if (actor->push(close_msg))
    {
        core::Scheduler::instance().dispatch(actor);
    }

    aegis::Log::instance().info("[Gate] Session Lifecycle Ended | FD: {}", client_fd);
    // 函数结束，conn 引用计数 -1，Socket 自动关闭
}

/**
 * @brief 网关监听协程
 */
core::DetachedTask server(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        aegis::Log::instance().critical("Syscall Failed: socket() | Errno: {}", errno);
        throw std::runtime_error("socket creation failed");
    }

    net::Socket listener(listen_fd);

    // [Robustness] SO_REUSEADDR 允许快速重启
    int val = 1;
    if (setsockopt(listener.native_handle(), SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
    {
        aegis::Log::instance().warn("setsockopt(SO_REUSEADDR) failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listener.native_handle(), (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        aegis::Log::instance().critical("Syscall Failed: bind() | Port: {} | Errno: {}", port, errno);
        throw std::runtime_error("bind failed");
    }

    if (listen(listener.native_handle(), 128) < 0)
    {
        aegis::Log::instance().critical("Syscall Failed: listen() | Errno: {}", errno);
        throw std::runtime_error("listen failed");
    }

    aegis::Log::instance().info("==========================================");
    aegis::Log::instance().info(" Aegis Gate Server Started");
    aegis::Log::instance().info(" Port: {}", port);
    aegis::Log::instance().info(" Mode: MVP (Single IO Thread + Worker Pool)");
    aegis::Log::instance().info("==========================================");

    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        // [Async IO] 异步 Accept
        int client_fd = co_await listener.accept((sockaddr *)&client_addr, &len);

        if (client_fd >= 0)
        {
            // 启动每个客户端的独立协程
            handle_client(client_fd);
        }
        else
        {
            aegis::Log::instance().warn("Accept returned invalid FD: {}", client_fd);
        }
    }
}

int main()
{
    try
    {
        // 1. Init Log
        aegis::Log::instance().init_config("logs/gate_server.log", "Gate");
        aegis::Log::instance().set_level(spdlog::level::warn);

        // 2. Init Scheduler (Worker Threads)
        int num_workers = std::thread::hardware_concurrency() - 1;
        if (num_workers < 1)
            num_workers = 1;

        aegis::Log::instance().info("[Init] Starting Scheduler with {} worker threads...", num_workers);
        core::Scheduler::instance().start(num_workers);

        // 3. Init IO Env
        aegis::Log::instance().info("[Init] Initializing IO Environment...");
        core::Env::instance().init();

        // 4. Start Server Coroutine
        server(8888);

        // 5. Enter Main Loop
        aegis::Log::instance().info("[Init] Entering Main IO Loop.");

        // [TODO] 添加信号处理 (Signal Handling) 以支持 core::Env::instance().stop()
        // 目前这是一个无限循环，直到被 Kill
        core::Env::instance().run();

        // [Unreachable in MVP]
        aegis::Log::instance().info("[Shutdown] Stopping Scheduler...");
        core::Scheduler::instance().stop();
    }
    catch (const std::exception &e)
    {
        std::cerr << "!!! FATAL ERROR !!! " << e.what() << std::endl;
        return 1;
    }
    return 0;
}