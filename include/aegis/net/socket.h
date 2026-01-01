#pragma once
#include <liburing.h>
#include <coroutine>
#include <string>
#include <stdexcept>
#include <cstring>
#include "aegis/core/env.h"

namespace aegis::net
{

    class Socket
    {
    public:
        Socket(int fd = -1) : fd_(fd) {}

        // 获取原生 fd
        int native_handle() const { return fd_; }

        // --- 异步操作 Awaiters ---

        // 异步接收
        struct AsyncRead
        {
            int result_ = 0;
            std::coroutine_handle<> handle_;
            int fd_;
            void *buf_;
            size_t len_;

            AsyncRead(int fd, void *buf, size_t len) : fd_(fd), buf_(buf), len_(len) {}

            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                handle_ = h;
                auto *ring = core::Env::instance().get_ring();
                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

                // 填充 io_uring 请求
                io_uring_prep_recv(sqe, fd_, buf_, len_, 0);

                // 【关键】把自己的指针存进去，等下 Env::run 里取出来
                // 注意：这里强制转换依赖于 Env.cpp 里的 BaseAwaiter 内存布局兼容
                // 在生产环境中，最好让 BaseAwaiter 成为公共头文件
                io_uring_sqe_set_data(sqe, this);
            }

            int await_resume()
            {
                if (result_ < 0)
                    throw std::runtime_error(std::strerror(-result_));
                return result_;
            }
        };

        // 异步发送
        struct AsyncWrite
        {
            int result_ = 0;
            std::coroutine_handle<> handle_;
            int fd_;
            const void *buf_;
            size_t len_;

            AsyncWrite(int fd, const void *buf, size_t len) : fd_(fd), buf_(buf), len_(len) {}
            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<> h)
            {
                handle_ = h;
                auto *ring = core::Env::instance().get_ring();
                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
                io_uring_prep_send(sqe, fd_, buf_, len_, 0);
                io_uring_sqe_set_data(sqe, this);
            }
            int await_resume()
            {
                if (result_ < 0)
                    throw std::runtime_error(std::strerror(-result_));
                return result_;
            }
        };

        // 异步连接 (Accept)
        struct AsyncAccept
        {
            int result_ = 0;
            std::coroutine_handle<> handle_;
            int server_fd_;
            struct sockaddr *client_addr_;
            socklen_t *client_len_;

            AsyncAccept(int fd, struct sockaddr *addr, socklen_t *len)
                : server_fd_(fd), client_addr_(addr), client_len_(len) {}

            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<> h)
            {
                handle_ = h;
                auto *ring = core::Env::instance().get_ring();
                struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
                io_uring_prep_accept(sqe, server_fd_, client_addr_, client_len_, 0);
                io_uring_sqe_set_data(sqe, this);
            }
            int await_resume()
            {
                if (result_ < 0)
                    throw std::runtime_error(std::strerror(-result_));
                return result_;
            }
        };

        // --- 便捷接口 ---

        [[nodiscard]] AsyncRead recv(void *buf, size_t len)
        {
            return AsyncRead(fd_, buf, len);
        }

        [[nodiscard]] AsyncWrite send(const void *buf, size_t len)
        {
            return AsyncWrite(fd_, buf, len);
        }

        [[nodiscard]] AsyncAccept accept(struct sockaddr *addr, socklen_t *len)
        {
            return AsyncAccept(fd_, addr, len);
        }

    private:
        int fd_;
    };

} // namespace aegis::net