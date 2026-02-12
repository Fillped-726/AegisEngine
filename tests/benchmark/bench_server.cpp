// bench_server.cpp
#include "aegis/core/env.h"
#include "aegis/net/acceptor.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"
#include <iostream>
#include <vector>

using namespace aegis;

// Echo 会话协程
core::DetachedTask echo_session(net::Socket socket)
{
    auto conn = std::make_shared<net::Connection>(std::move(socket));

    try
    {
        while (true)
        {
            // 1. 读取数据 (零拷贝，直接从 Socket 读到 Packet)
            auto packet = co_await conn->read_packet();

            if (!packet)
                break; // 连接关闭

            // 2. 原样发送回去 (Move 语义，零拷贝)
            conn->send(std::move(packet));

            // 3. 触发发送 (在真实业务中通常由外层 Loop 触发 flush，这里我们手动 flush 确保低延迟)
            // 注意：在高吞吐场景下，频繁 flush 会增加 syscall，
            // 但因为 Connection 内部有 batching 和 is_flushing 锁，这里调用是安全的。
            conn->flush();
        }
    }
    catch (const std::exception &e)
    {
        // 忽略连接重置等常规网络错误，避免刷屏
    }
}

// 监听协程
core::DetachedTask accept_loop(int port)
{
    try
    {
        net::Acceptor acceptor(port);
        Log::instance().warn(">>> BenchServer running on port {} <<<", port);
        Log::instance().warn(">>> Press Ctrl+C to stop <<<");

        while (true)
        {
            // 接受连接
            auto socket = co_await acceptor.accept();
            if (socket.is_valid())
            {
                // 启动会话
                echo_session(std::move(socket));
            }
        }
    }
    catch (const std::exception &e)
    {
        Log::instance().critical("Acceptor failed: {}", e.what());
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    // 1. 日志级别设为 WARN，关闭 Console 刷屏，这对性能至关重要！
    Log::instance().set_level(spdlog::level::warn);

    // 2. 初始化 io_uring
    // 队列深度设为 4096 (应对高并发)
    core::Env::instance().init(4096);

    int port = 8888;
    if (argc > 1)
        port = std::atoi(argv[1]);

    // 3. 启动监听
    accept_loop(port);

    // 4. 进入事件循环
    core::Env::instance().run();

    return 0;
}