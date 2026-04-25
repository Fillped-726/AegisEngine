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

    /**
     * @brief Pure Gateway — only responsible for network I/O and session management.
     *
     * No business logic includes (SceneActor, NpcActor, RoomManager).
     * init() sets up Log, Scheduler, and fd limit only.
     * handle_session() binds a Socket to a new PlayerActor.
     */
    class GateServer
    {
    public:
        GateServer() = default;
        ~GateServer();

        // 禁止拷贝
        GateServer(const GateServer &) = delete;
        GateServer &operator=(const GateServer &) = delete;

        // 初始化：仅拉起 Log、Scheduler、tune_fd_limit
        void init(const std::string &config_path, int num_workers = 0);

        // 运行：启动主 IO 循环 (阻塞)
        void run(int port);

        // 停止：优雅退出
        void stop();

    private:
        // --- 协程循环 ---
        aegis::core::DetachedTask accept_loop(int port);
        aegis::core::DetachedTask handle_session(net::Socket client_socket);
        void dispatch_to_actor(core::Actor *actor, core::ActorMessage *msg);
    };

} // namespace aegis::gate
