#pragma once

#include "aegis/core/task.h"
#include "aegis/net/socket.h"
#include <string>
#include <netinet/in.h>

namespace aegis::net
{

    /**
     * @brief 专门负责 TCP 监听与连接接受
     * 遵循 RAII 原则，析构时自动关闭监听 Socket
     */
    class Acceptor
    {
    public:
        // 构造函数立即绑定并监听，抛出异常如果失败
        explicit Acceptor(int port, const std::string &ip = "0.0.0.0");
        ~Acceptor() = default;

        // 禁用拷贝，允许移动
        Acceptor(const Acceptor &) = delete;
        Acceptor &operator=(const Acceptor &) = delete;
        Acceptor(Acceptor &&) = default;
        Acceptor &operator=(Acceptor &&) = default;

        /**
         * @brief 异步等待新连接
         * @return Task<Socket> 返回一个新的已连接 Socket
         */
        core::Task<Socket> accept();

        // 获取原生句柄
        [[nodiscard]] int native_handle() const { return listener_.native_handle(); }
        [[nodiscard]] int port() const { return port_; }

    private:
        void init_listener(const std::string &ip);
        void optimize_client_socket(int fd);

        Socket listener_;
        int port_;
        sockaddr_in addr_{};
    };

} // namespace aegis::net