#include "aegis/net/acceptor.h"
#include "aegis/common/aegisLog.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdexcept>

namespace aegis::net
{

    Acceptor::Acceptor(int port, const std::string &ip)
        : port_(port)
    {
        // 1. 创建 Socket
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            Log::instance().error("Acceptor: socket creation failed. errno={}", errno);
            throw std::runtime_error("Acceptor socket creation failed");
        }

        listener_ = Socket(fd);
        init_listener(ip);
    }

    void Acceptor::init_listener(const std::string &ip)
    {
        int fd = listener_.native_handle();
        int opt = 1;

        // 1. 允许地址重用 (Essential for server restart)
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        {
            Log::instance().warn("Acceptor: Failed to set SO_REUSEADDR");
        }

        // // 2. 设置非阻塞 (Essential for co_await)
        // int flags = ::fcntl(fd, F_GETFL, 0);
        // ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        // 3. Bind
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port_);
        if (inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0)
        {
            if (ip == "0.0.0.0" || ip.empty())
            {
                addr_.sin_addr.s_addr = INADDR_ANY;
            }
            else
            {
                throw std::runtime_error("Acceptor: Invalid binding IP address: " + ip);
            }
        }

        if (::bind(fd, (sockaddr *)&addr_, sizeof(addr_)) < 0)
        {
            Log::instance().critical("Acceptor: bind failed on port {}. errno={}", port_, errno);
            throw std::runtime_error("Acceptor bind failed");
        }

        if (port_ == 0)
        {
            socklen_t len = sizeof(addr_);
            if (::getsockname(fd, (sockaddr *)&addr_, &len) == 0)
            {
                port_ = ntohs(addr_.sin_port); // 更新成员变量
            }
        }

        // 4. Listen
        if (::listen(fd, 4096) < 0)
        {
            Log::instance().critical("Acceptor: listen failed. errno={}", errno);
            throw std::runtime_error("Acceptor listen failed");
        }

        Log::instance().info("Acceptor listening on {}:{}", ip, port_);
    }

    core::Task<Socket> Acceptor::accept()
    {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        // 使用 Socket 类的原始 accept 协程
        // 注意：这里的 Socket::accept 返回的是 int fd
        int client_fd = co_await listener_.accept((sockaddr *)&client_addr, &len);

        if (client_fd >= 0)
        {
            optimize_client_socket(client_fd);

            // 打印客户端 IP (可选)
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
            Log::instance().info("Acceptor: New connection from {}:{}", ip_str, ntohs(client_addr.sin_port));

            co_return Socket(client_fd);
        }
        else
        {
            // 错误处理交给调用者，或者返回一个无效 Socket
            // 这里我们抛出异常或者返回无效 Socket 都可以，
            // 考虑到协程异常处理，返回无效 Socket 并在上层判断更安全。
            co_return Socket(-1);
        }
    }

    void Acceptor::optimize_client_socket(int fd)
    {
        int opt = 1;

        // // 1. 设置非阻塞
        // int flags = ::fcntl(fd, F_GETFL, 0);
        // ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        // 2. 禁用 Nagle 算法 (低延迟)
        if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0)
        {
            Log::instance().warn("Acceptor: Failed to set TCP_NODELAY");
        }

        // 3. KeepAlive
        if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0)
        {
            Log::instance().warn("Acceptor: Failed to set SO_KEEPALIVE");
        }

        // 4. Linux TCP Keepalive params (Optional)
        int idle = 60, intvl = 10, cnt = 3;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    }

} // namespace aegis::net