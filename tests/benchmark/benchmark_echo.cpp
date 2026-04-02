#include <iostream>
#include <memory>
#include <csignal>
#include <thread>

#include "aegis/core/scheduler.h"
#include "aegis/core/worker.h"
#include "aegis/net/acceptor.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"
#include "aegis/common/tools.h" // 假设里面有 bind_to_core 等工具函数

using namespace aegis;

// 全局运行标志，用于优雅退出
std::atomic<bool> g_running{true};

void signal_handler(int)
{
    g_running = false;
}

// ============================================================================
// [核心协程]：处理单个连接的生命周期 (运行在目标 Worker 的上下文中)
// ============================================================================
core::DetachedTask handle_connection(net::Socket sock)
{
    // 将裸的 Socket 包装成具备粘包处理、批量发送能力的 Connection
    auto conn = std::make_shared<net::Connection>(std::move(sock));

    // 获取当前所在的 Worker ID，用于日志观察负载均衡效果 (压测时可注释掉日志)
    int worker_id = core::Worker::get_current_id();
    Log::instance().debug("New connection accepted and routed to Worker {}", worker_id);

    try
    {
        while (true)
        {
            // 1. 异步等待并读取完整的包 (框架已自动处理 Length-Value 粘包)
            auto pkt = co_await conn->read_packet();

            // 如果 pkt 为空，说明对端关闭了连接或发生网络错误
            if (!pkt)
            {
                break;
            }

            // 2. 极致零拷贝 Echo：直接把收到的包原封不动地塞进发送队列
            // 注意：这里调用的是 conn->send(std::move(pkt))，业务层零耗时！
            // 底层的 outbox_batcher 会自动把它们收集起来，用 writev 批量发送
            conn->send(std::move(pkt));
        }
    }
    catch (const std::exception &e)
    {
        Log::instance().error("Connection error on Worker {}: {}", worker_id, e.what());
    }

    Log::instance().debug("Connection closed on Worker {}", worker_id);
}

// ============================================================================
// [Acceptor 协程]：专门负责监听端口，并把连接负载均衡到所有 CPU 核心
// ============================================================================
core::DetachedTask start_accept_loop(int port, int num_workers)
{
    net::Acceptor acceptor(port, "0.0.0.0");
    uint64_t counter = 0;

    Log::instance().info("Echo Server listening on port {}...", port);

    while (g_running.load(std::memory_order_relaxed))
    {
        // 1. 异步等待新连接
        net::Socket client = co_await acceptor.accept();
        if (!client.is_valid())
        {
            continue;
        }

        // 2. 跨核负载均衡 (核心压测逻辑)
        // 使用简单的 Round-Robin 算法分配目标 CPU 核心
        int target_worker_id = counter++ % num_workers;
        auto *worker = core::Scheduler::instance().get_worker(target_worker_id);

        if (worker)
        {
            // 3. 将新建立的 Socket 所有权(Move) 通过无锁队列转移给目标 Worker
            worker->post_custom_task([sock = std::move(client)]() mutable
                                     {
                // 这个 Lambda 已经在目标 Worker 的线程里执行了！
                // 此时拉起处理协程，该协程将终身绑定在这个 Worker 的 io_uring 上
                handle_connection(std::move(sock)); });
        }
    }
}

// ============================================================================
// 主函数：启动引擎，初始化控制面
// ============================================================================
int main()
{
    // 注册信号量，实现 Ctrl+C 优雅停机
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    tune_fd_limit();

    // 1. 确定工作线程数 (留一个物理核给 OS 和其他进程，防止饥饿)
    int num_cores = std::thread::hardware_concurrency() / 2;
    int num_workers = std::max(1, num_cores - 1);

    Log::instance().set_level(spdlog::level::warn);
    Log::instance().info("Starting Aegis Engine with {} Worker(s)...", num_workers);

    // 2. 启动全局调度器 (拉起 Thread-per-Core 线程池)
    core::Scheduler::instance().start(num_workers);

    // 3. 将 Acceptor 任务丢给 Worker 0 专门处理
    // (在超高并发下，如果有 SO_REUSEPORT 支持，可以每个 Worker 跑一个 Acceptor，
    // 但目前一个 Acceptor 分发给多 Worker 的模式已经能应对绝大多数压测)
    auto *worker0 = core::Scheduler::instance().get_worker(0);
    if (worker0)
    {
        worker0->post_custom_task([num_workers]()
                                  { start_accept_loop(8888, num_workers); });
    }

    // 4. 阻塞主线程，等待退出信号
    while (g_running.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 5. 优雅关闭所有连接和 Event Loop
    Log::instance().info("Shutting down Aegis Engine...");
    core::Scheduler::instance().stop();

    return 0;
}