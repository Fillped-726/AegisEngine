#pragma once

#include <memory>
#include <string>
#include "aegis/core/task.h"

// 前向声明，减少编译依赖
namespace aegis::core
{
    class SceneActor;
}

namespace aegis::gate
{
    class GateServer
    {
    public:
        GateServer() = default;
        ~GateServer();

        // 禁止拷贝
        GateServer(const GateServer &) = delete;
        GateServer &operator=(const GateServer &) = delete;

        // 初始化：加载配置、场景、逻辑、线程池
        void init(const std::string &config_path, int num_workers = 0);

        // 运行：启动主 IO 循环 (阻塞)
        void run(int port);

        // 停止：优雅退出
        void stop();

    private:
        // --- 基础设施方法 (Infrastructure) ---
        void start_timer();
        void init_scene();

        // --- 协程循环 ---
        aegis::core::DetachedTask accept_loop(int port);
        aegis::core::DetachedTask handle_session(int client_fd);
        aegis::core::DetachedTask timer_loop();

    private:
        // 核心资源的所有权
        std::shared_ptr<aegis::core::SceneActor> scene_;
    };
}