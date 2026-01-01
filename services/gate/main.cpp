#include <iostream>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aegis/core/env.h"
#include "aegis/core/task.h"
#include "aegis/net/socket.h"

using namespace aegis;

// 处理单个客户端连接
core::Task handle_client(int client_fd)
{
    net::Socket conn(client_fd);
    std::vector<char> buffer(1024);

    try
    {
        while (true)
        {
            // 1. 异步读取
            int n = co_await conn.recv(buffer.data(), buffer.size());
            if (n <= 0)
                break; // 断开连接

            // 2. 异步回显
            co_await conn.send(buffer.data(), n);
            std::cout << "[Echo] " << n << " bytes" << std::endl;
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Client error: " << e.what() << std::endl;
    }
    close(client_fd);
}

// 服务器监听循环
core::Task server(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        throw std::runtime_error("socket error");

    int val = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind error");

    if (listen(listen_fd, 128) < 0)
        throw std::runtime_error("listen error");

    net::Socket listener(listen_fd);
    std::cout << "Gate Server listening on " << port << "..." << std::endl;

    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        // 1. 异步 Accept
        int client_fd = co_await listener.accept((sockaddr *)&client_addr, &len);
        std::cout << "New client connected: " << client_fd << std::endl;

        // 2. 启动协程处理客户端 (Fire and Forget)
        handle_client(client_fd);
    }
}

int main()
{
    try
    {
        // 1. 初始化 io_uring
        core::Env::instance().init();

        // 2. 启动服务器协程
        server(8888);

        // 3. 进入事件循环
        core::Env::instance().run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}