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
#include "aegis/common/unique_fd.h"

namespace aegis::net
{
    using UniqueFd = aegis::common::UniqueFd;

    class Socket
    {
    public:
        explicit Socket(int fd = -1) : fd_(fd) {}
        Socket(Socket &&other) noexcept = default;
        Socket &operator=(Socket &&other) noexcept = default;
        int native_handle() const { return fd_.get(); }
        void close() { fd_.reset(); }
        bool is_valid() const { return fd_.get() >= 0; }

        // --- 核心辅助：获取 SQE (带自动 Flush) ---
        static struct io_uring_sqe *get_sqe_safe(struct io_uring *ring)
        {
            struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

            if (!sqe)
            {
                // 提交当前积压的任务
                io_uring_submit(ring);

                // 再试一次
                sqe = io_uring_get_sqe(ring);

                // 如果还满，说明内核处理不过来了，或者 SQ 设置太小
                if (!sqe) [[unlikely]]
                {
                    throw std::runtime_error("io_uring SQ ring full (fatal)");
                }
            }
            return sqe;
        }

        // --- Awaiters ---

        struct AsyncRead : public core::BaseAwaiter
        {
            int fd_;
            void *buf_;
            size_t len_;
            int flags_;

            AsyncRead(int fd, void *buf, size_t len, int flags = 0) : fd_(fd), buf_(buf), len_(len), flags_(flags) {}
            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                handle = h;
                auto *ring = core::Env::instance().native_handle();

                struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);

                io_uring_prep_recv(sqe, fd_, buf_, len_, flags_);

                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
            }

            int await_resume()
            {
                if (result < 0)
                    throw std::system_error(-result, std::system_category(), "AsyncRead failed");
                return result;
            }
        };

        struct AsyncReadV : public core::BaseAwaiter
        {
            int fd_;
            const struct iovec *iovs_;
            size_t count_;

            AsyncReadV(int fd, std::span<struct iovec> iovs)
                : fd_(fd), iovs_(iovs.data()), count_(iovs.size()) {}

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                handle = h;
                auto *ring = core::Env::instance().native_handle();

                struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);

                // 使用 readv 填充不连续的 RingBuffer
                io_uring_prep_readv(sqe, fd_, iovs_, count_, 0);
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
            }

            int await_resume()
            {
                if (result < 0)
                    throw std::system_error(-result, std::system_category(), "AsyncReadV failed");
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
                auto *ring = core::Env::instance().native_handle();

                struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);

                io_uring_prep_send(sqe, fd_, buf_, len_, 0);
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
            }

            int await_resume()
            {
                if (result < 0)
                    throw std::system_error(-result, std::system_category(), "AsyncWrite failed");
                return result;
            }
        };

        struct AsyncWriteV : public core::BaseAwaiter
        {
            int fd_;
            const struct iovec *iovs_;
            size_t count_;

            // 使用 span 传递，更安全现代
            AsyncWriteV(int fd, std::span<struct iovec> iov_span)
                : fd_(fd), iovs_(iov_span.data()), count_(iov_span.size()) {}

            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                handle = h;
                auto *ring = core::Env::instance().native_handle();

                struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);

                // [Core] 使用 writev
                io_uring_prep_writev(sqe, fd_, iovs_, count_, 0);

                // 绑定 User Data
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));

                io_uring_submit(ring); // 提交以提高发送及时性
            }

            int await_resume()
            {
                if (result < 0)
                    throw std::system_error(-result, std::system_category(), "AsyncWriteV failed");
                return result; // 返回实际发送的字节数
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
                auto *ring = core::Env::instance().native_handle();

                struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);

                io_uring_prep_accept(sqe, server_fd_, client_addr_, client_len_, 0);
                io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
            }

            int await_resume()
            {
                if (result < 0)
                    throw std::system_error(-result, std::system_category(), "AsyncAccept failed");
                return result;
            }
        };

        [[nodiscard]] AsyncRead recv(void *buf, size_t len, int flags = 0) { return AsyncRead(fd_.get(), buf, len, flags); }
        [[nodiscard]] AsyncReadV recv(std::span<struct iovec> iovs) { return AsyncReadV(fd_.get(), iovs); }
        [[nodiscard]] AsyncWrite send(const void *buf, size_t len) { return AsyncWrite(fd_.get(), buf, len); }
        [[nodiscard]] AsyncWriteV send(std::span<struct iovec> iovs) { return AsyncWriteV(fd_.get(), iovs); }
        [[nodiscard]] AsyncAccept accept(struct sockaddr *addr, socklen_t *len) { return AsyncAccept(fd_.get(), addr, len); }

    private:
        UniqueFd fd_;
    };
}