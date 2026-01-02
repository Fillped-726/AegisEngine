#pragma once
#include <liburing.h>
#include <coroutine>
#include <stdexcept>
#include <cstring>
#include <unistd.h>
#include <utility>
#include "aegis/core/env.h"
#include "aegis/core/awaiter.h"

namespace aegis::net
{

    // --- RAII Wrapper for File Descriptor ---
    class UniqueFd
    {
    public:
        UniqueFd(int fd = -1) : fd_(fd) {}
        ~UniqueFd()
        {
            if (fd_ >= 0)
                ::close(fd_);
        }

        // 禁用拷贝 (防止两个对象 close 同一个 fd)
        UniqueFd(const UniqueFd &) = delete;
        UniqueFd &operator=(const UniqueFd &) = delete;

        // 允许移动 (把所有权转移给别人)
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
        // 释放所有权 (比如 accept 返回时)
        int release() { return std::exchange(fd_, -1); }
        // 重新赋值
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
        // 接管 fd 的所有权
        explicit Socket(int fd = -1) : fd_(fd) {}

        // 移动构造函数 (必需，因为 UniqueFd 不可拷贝)
        Socket(Socket &&other) noexcept = default;
        Socket &operator=(Socket &&other) noexcept = default;

        int native_handle() const { return fd_.get(); }

        // --- 安全重构后的 Awaiters ---

        // 1. 继承 BaseAwaiter
        struct AsyncRead : public core::BaseAwaiter
        {
            int fd_;
            void *buf_;
            size_t len_;

            AsyncRead(int fd, void *buf, size_t len) : fd_(fd), buf_(buf), len_(len) {}

            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<> h)
            {
                handle = h; // 存入基类
                auto *ring = core::Env::instance().get_ring();
                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
                io_uring_prep_recv(sqe, fd_, buf_, len_, 0);

                // 安全修正：存入 BaseAwaiter 指针
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
            }
            int await_resume()
            {
                if (result < 0)
                    throw std::runtime_error(std::strerror(-result));
                return result; // 返回基类里的 result
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
                auto *ring = core::Env::instance().get_ring();
                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
                io_uring_prep_send(sqe, fd_, buf_, len_, 0);
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
            }
            int await_resume()
            {
                if (result < 0)
                    throw std::runtime_error(std::strerror(-result));
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
                auto *ring = core::Env::instance().get_ring();
                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
                io_uring_prep_accept(sqe, server_fd_, client_addr_, client_len_, 0);
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
            }
            int await_resume()
            {
                if (result < 0)
                    throw std::runtime_error(std::strerror(-result));
                return result;
            }
        };

        // --- 接口 ---
        [[nodiscard]] AsyncRead recv(void *buf, size_t len)
        {
            return AsyncRead(fd_.get(), buf, len);
        }
        [[nodiscard]] AsyncWrite send(const void *buf, size_t len)
        {
            return AsyncWrite(fd_.get(), buf, len);
        }
        [[nodiscard]] AsyncAccept accept(struct sockaddr *addr, socklen_t *len)
        {
            return AsyncAccept(fd_.get(), addr, len);
        }

    private:
        UniqueFd fd_; // RAII 管理
    };

} // namespace aegis::net