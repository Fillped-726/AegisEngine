#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "aegis/core/task.h"
#include "aegis/core/actor_registry.h"
#include "aegis/net/socket.h"

namespace aegis::core
{
    class Actor;
    class ActorMessage;
}

namespace aegis::gate
{
    // 会话上下文
    struct ClientSession
    {
        // ActorID (对应 Registry 中的 ID)
        core::ActorID actor_id;
        bool logged_in = false;
    };
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

        inline static core::ActorID g_DefaultSceneID;

    private:
        // --- 协程循环 ---
        aegis::core::DetachedTask accept_loop(int port);
        aegis::core::DetachedTask handle_session(net::Socket client_socket);
        void dispatch_to_actor(core::Actor *actor, core::ActorMessage *msg);

    private:
        core::ActorID room_manager_id_;
    };
}