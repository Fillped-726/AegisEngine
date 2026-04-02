#include <iostream>
#include <csignal>
#include "gate_server.h"

int main()
{
    try
    {
        // 实例化门面
        aegis::gate::GateServer server;

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        {
            // 理论上不会失败，但严谨起见可以处理
            return 1;
        }

        // 1. 初始化 (日志、调度器、IO环)
        // 参数可以从命令行解析，这里先硬编码
        server.init("logs/gate_server.log", 4);

        // 2. 运行 (启动监听、定时器，阻塞住)
        server.run(8888);

        // run() 内部是 while(true)，除非抛出异常，否则不会走到这里
    }
    catch (const std::exception &e)
    {
        std::cerr << "!!! FATAL ERROR !!! " << e.what() << std::endl;
        return 1;
    }

    return 0;
}