// acceptor.h
#pragma once

#include "aegis/core/task.h"  // [DEPENDENCY: aegis::core::Task]
#include "aegis/net/socket.h" // [DEPENDENCY: aegis::net::Socket]
#include <string>
#include <netinet/in.h>

namespace aegis::net
{
    // [INTENT: Async TCP listener facade; RAII lifecycle management]
    class Acceptor
    {
    public:
        // [STATE_MUTATION: Immediate bind and listen; throws on failure]
        explicit Acceptor(int port, const std::string &ip = "0.0.0.0");
        ~Acceptor() = default;

        // [CONSTRAINT: Move-only semantics]
        Acceptor(const Acceptor &) = delete;
        Acceptor &operator=(const Acceptor &) = delete;
        Acceptor(Acceptor &&) = default;
        Acceptor &operator=(Acceptor &&) = default;

        // [INTENT: Coroutine suspension point yielding connected peer socket]
        core::Task<Socket> accept();

        // [STATE: Expose underlying POSIX listener fd]
        [[nodiscard]] int native_handle() const { return listener_.native_handle(); }
        [[nodiscard]] int port() const { return port_; }

    private:
        // [STATE_MUTATION: Execute socket/bind/listen syscall sequence]
        void init_listener(const std::string &ip);

        // [STATE_MUTATION: Apply performance tuning (e.g., TCP_NODELAY, O_NONBLOCK) to accepted fd]
        void optimize_client_socket(int fd);

        // [STATE: RAII listener handle]
        Socket listener_;
        int port_;
        // [STATE: Bound IPv4 topology]
        sockaddr_in addr_{};
    };

} // namespace aegis::net