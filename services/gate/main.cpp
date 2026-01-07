#include <iostream>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aegis/core/env.h"
#include "aegis/core/task.h"
#include "aegis/net/socket.h"
#include "aegis/net/connection.h"
#include "common.pb.h"

using namespace aegis;

// 处理单个客户端连接
core::DetachedTask handle_client(int client_fd)
{
    // 1. 创建 Connection (接管 socket)
    net::Connection conn{net::Socket(client_fd)};

    std::cout << "[Gate] New connection established." << std::endl;

    try
    {
        while (true)
        {
            auto packet = co_await conn.read_packet();
            if (!packet)
                break;

            // 1. 直接获取 ID (无需手动移位)
            uint32_t msg_id = packet->msg_id();

            std::cout << "[Recv] MsgID: " << msg_id << std::endl;

            if (msg_id == aegis::proto::CS_LOGIN_REQ)
            {
                aegis::proto::LoginReq req;
                // 2. 优雅解析 (无需传 size 和 data指针)
                if (packet->parse(req))
                {
                    std::cout << "  Login UID: " << req.uid() << std::endl;
                }
            }
            // 回显包
            co_await conn.send_packet(*packet);

            std::cout << "[Send] Echo packet back." << std::endl;
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Client error: " << e.what() << std::endl;
    }
}

// 服务器监听循环
core::DetachedTask server(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        throw std::runtime_error("socket error");

    net::Socket listener(listen_fd);
    int val = 1;
    setsockopt(listener.native_handle(), SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listener.native_handle(), (sockaddr *)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind error");

    if (listen(listener.native_handle(), 128) < 0)
        throw std::runtime_error("listen error");

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