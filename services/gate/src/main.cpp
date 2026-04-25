#include <iostream>
#include <csignal>
#include "gate_server.h"
#include "aegis/core/game_app.h"

int main()
{
    try
    {
        // 1. 实例化网关 (纯网络层)
        aegis::gate::GateServer server;

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        {
            return 1;
        }

        // 2. 初始化网关 (日志、调度器、tune_fd_limit)
        //    内部会先 load_handlers() 注册所有业务处理器
        server.init("logs/gate_server.log", 4);

        // 3. 启动业务层 (GameApp 接管 RoomManager、主城创建、NPC 刷怪)
        aegis::core::GameApp::instance().init();

        // 4. 运行网关 (accept 循环 + 事件驱动，阻塞)
        server.run(8888);

        // run() 会阻塞，除非异常不会走到这里
    }
    catch (const std::exception &e)
    {
        std::cerr << "!!! FATAL ERROR !!! " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
