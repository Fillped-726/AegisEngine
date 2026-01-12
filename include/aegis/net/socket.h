#pragma once

#include <liburing.h>
#include <coroutine>
#include <system_error>
#include <cstring>
#include <unistd.h>
#include <utility>
#include <mutex> // [Added]

#include "aegis/core/env.h"
#include "aegis/core/awaiter.h"
#include "aegis/common/aegisLog.h"

namespace aegis::net
{
    // ... (UniqueFd 类保持不变) ...
    class UniqueFd
    {
    public:
        UniqueFd(int fd = -1) : fd_(fd) {}
        ~UniqueFd()
        {
            if (fd_ >= 0)
                ::close(fd_);
        }
        UniqueFd(const UniqueFd &) = delete;
        UniqueFd &operator=(const UniqueFd &) = delete;
        UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
        UniqueFd &operator=(UniqueFd &&other) noexcept
        {
            if (this != &other)
            {
                if (fd_ >= 0)
                    ::close(fd_);
                fd_ = std::exchange(other.fd_, -1);
            }
            return *this;
        }
        int get() const { return fd_; }
        int release() { return std::exchange(fd_, -1); }
        explicit operator bool() const { return fd_ >= 0; }
        void reset(int fd = -1)
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = fd;
        }

    private:
        int fd_;
    };

    class Socket
    {
    public:
        explicit Socket(int fd = -1) : fd_(fd) {}
        Socket(Socket &&other) noexcept = default;
        Socket &operator=(Socket &&other) noexcept = default;
        int native_handle() const { return fd_.get(); }

        // --- Awaiters ---

        struct AsyncRead : public core::BaseAwaiter
        {
            int fd_;
            void *buf_;
            size_t len_;

            AsyncRead(int fd, void *buf, size_t len) : fd_(fd), buf_(buf), len_(len) {}
            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                handle = h;
                auto &env = core::Env::instance();
                auto *ring = env.native_handle(); // 使用 native_handle

                // [Fix] 加锁保护 SQ
                std::lock_guard<std::mutex> lock(env.get_submission_mutex());

                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
                if (!sqe)
                {
                    io_uring_submit(ring); // 尝试刷出空间
                    sqe = io_uring_get_sqe(ring);
                    if (!sqe)
                        throw std::runtime_error("SQ full");
                }

                io_uring_prep_recv(sqe, fd_, buf_, len_, 0);
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
                io_uring_submit(ring);
            }

            int await_resume()
            {
                if (result < 0)
                    throw std::system_error(-result, std::system_category(), "AsyncRead failed");
                return result;
            }
        };

        struct AsyncWrite : public core::BaseAwaiter
        {
            int fd_;
            const void *buf_;
            size_t len_;

            AsyncWrite(int fd, const void *buf, size_t len) : fd_(fd), buf_(buf), len_(len) {}
            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                handle = h;
                auto &env = core::Env::instance();
                auto *ring = env.native_handle();

                // [Fix] 加锁保护 SQ
                // 这是防止 Worker 线程和 IO 线程冲突的关键
                std::lock_guard<std::mutex> lock(env.get_submission_mutex());

                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
                if (!sqe)
                {
                    io_uring_submit(ring);
                    sqe = io_uring_get_sqe(ring);
                    if (!sqe)
                        throw std::runtime_error("SQ full");
                }

                io_uring_prep_send(sqe, fd_, buf_, len_, 0);
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
                io_uring_submit(ring);
            }

            int await_resume()
            {
                if (result < 0)
                    throw std::system_error(-result, std::system_category(), "AsyncWrite failed");
                return result;
            }
        };

        struct AsyncAccept : public core::BaseAwaiter
        {
            int server_fd_;
            struct sockaddr *client_addr_;
            socklen_t *client_len_;

            AsyncAccept(int fd, struct sockaddr *addr, socklen_t *len)
                : server_fd_(fd), client_addr_(addr), client_len_(len) {}
            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                handle = h;
                auto &env = core::Env::instance();
                auto *ring = env.native_handle();

                // [Fix] 加锁保护 SQ
                std::lock_guard<std::mutex> lock(env.get_submission_mutex());

                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
                if (!sqe)
                {
                    io_uring_submit(ring);
                    sqe = io_uring_get_sqe(ring);
                    if (!sqe)
                        throw std::runtime_error("SQ full");
                }

                io_uring_prep_accept(sqe, server_fd_, client_addr_, client_len_, 0);
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
                io_uring_submit(ring);
            }

            int await_resume()
            {
                if (result < 0)
                    throw std::system_error(-result, std::system_category(), "AsyncAccept failed");
                return result;
            }
        };

        [[nodiscard]] AsyncRead recv(void *buf, size_t len) { return AsyncRead(fd_.get(), buf, len); }
        [[nodiscard]] AsyncWrite send(const void *buf, size_t len) { return AsyncWrite(fd_.get(), buf, len); }
        [[nodiscard]] AsyncAccept accept(struct sockaddr *addr, socklen_t *len) { return AsyncAccept(fd_.get(), addr, len); }

    private:
        UniqueFd fd_;
    };
}