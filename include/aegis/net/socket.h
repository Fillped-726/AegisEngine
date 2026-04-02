#pragma once

#include <liburing.h> // [DEPENDENCY: Linux io_uring]
#include <coroutine>  // [DEPENDENCY: C++20 coroutines]
#include <system_error>
#include <span>
#include <vector>

#include "aegis/core/awaiter.h" // [DEPENDENCY: aegis::core::BaseAwaiter]
#include "aegis/common/unique_fd.h"

namespace aegis::net
{
    using UniqueFd = aegis::common::UniqueFd;

    // [INTENT: io_uring async IO facade via C++20 coroutines]
    class Socket
    {
    public:
        // [STATE_MUTATION: Adopt RAII fd handle]
        explicit Socket(int fd = -1) : fd_(fd) {}
        Socket(Socket &&other) noexcept = default;
        Socket &operator=(Socket &&other) noexcept = default;

        int native_handle() const { return fd_.get(); }
        void close() { fd_.reset(); }
        bool is_valid() const { return fd_.get() >= 0; }

        // [STATE_MUTATION: Acquire SQE; force submission ring flush on overflow]
        static struct io_uring_sqe *get_sqe_safe(struct io_uring *ring);

        // [INTENT: io_uring opcode awaitables; map CQE results to coroutine resumption]

        // [STATE: Read scalar buffer]
        struct AsyncRead : public core::BaseAwaiter
        {
            int fd_;
            void *buf_;
            size_t len_;
            int flags_;

            AsyncRead(int fd, void *buf, size_t len, int flags = 0);
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h);
            int await_resume();
        };

        // [STATE: Read scatter-gather vectors]
        struct AsyncReadV : public core::BaseAwaiter
        {
            int fd_;
            const struct iovec *iovs_;
            size_t count_;

            AsyncReadV(int fd, std::span<struct iovec> iovs);
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h);
            int await_resume();
        };

        // [STATE: Write scalar buffer]
        struct AsyncWrite : public core::BaseAwaiter
        {
            int fd_;
            const void *buf_;
            size_t len_;

            AsyncWrite(int fd, const void *buf, size_t len);
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h);
            int await_resume();
        };

        // [STATE: Write scatter-gather vectors]
        struct AsyncWriteV : public core::BaseAwaiter
        {
            int fd_;
            const struct iovec *iovs_;
            size_t count_;

            AsyncWriteV(int fd, std::span<struct iovec> iov_span);
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h);
            int await_resume();
        };

        // [STATE: Async peer acceptance]
        struct AsyncAccept : public core::BaseAwaiter
        {
            int server_fd_;
            struct sockaddr *client_addr_;
            socklen_t *client_len_;

            AsyncAccept(int fd, struct sockaddr *addr, socklen_t *len);
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h);
            int await_resume();
        };

        // [INTENT: Zero-cost awaiter factories for co_await suspension points]

        [[nodiscard]] AsyncRead recv(void *buf, size_t len, int flags = 0)
        {
            return AsyncRead(fd_.get(), buf, len, flags);
        }

        [[nodiscard]] AsyncReadV recv(std::span<struct iovec> iovs)
        {
            return AsyncReadV(fd_.get(), iovs);
        }

        [[nodiscard]] AsyncWrite send(const void *buf, size_t len)
        {
            return AsyncWrite(fd_.get(), buf, len);
        }

        [[nodiscard]] AsyncWriteV send(std::span<struct iovec> iovs)
        {
            return AsyncWriteV(fd_.get(), iovs);
        }

        [[nodiscard]] AsyncAccept accept(struct sockaddr *addr, socklen_t *len)
        {
            return AsyncAccept(fd_.get(), addr, len);
        }

    private:
        // [STATE: Managed POSIX handle]
        UniqueFd fd_;
    };
}