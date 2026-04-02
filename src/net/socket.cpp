#include "aegis/net/socket.h"
#include "aegis/core/worker.h"
#include <stdexcept>

namespace aegis::net
{
    // --- Socket 静态辅助方法 ---
    struct io_uring_sqe *Socket::get_sqe_safe(struct io_uring *ring)
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (!sqe)
        {
            io_uring_submit(ring);
            sqe = io_uring_get_sqe(ring);
            if (!sqe) [[unlikely]]
            {
                throw std::runtime_error("io_uring SQ ring full (fatal)");
            }
        }
        return sqe;
    }

    // --- AsyncRead ---
    Socket::AsyncRead::AsyncRead(int fd, void *buf, size_t len, int flags)
        : fd_(fd), buf_(buf), len_(len), flags_(flags) {}

    void Socket::AsyncRead::await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        auto *ring = core::t_current_worker->ring();
        struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);
        io_uring_prep_recv(sqe, fd_, buf_, len_, flags_);
        io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
    }

    int Socket::AsyncRead::await_resume()
    {
        if (result < 0)
            throw std::system_error(-result, std::system_category(), "AsyncRead failed");
        return result;
    }

    // --- AsyncReadV ---
    Socket::AsyncReadV::AsyncReadV(int fd, std::span<struct iovec> iovs)
        : fd_(fd), iovs_(iovs.data()), count_(iovs.size()) {}

    void Socket::AsyncReadV::await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        auto *ring = core::t_current_worker->ring();
        struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);
        io_uring_prep_readv(sqe, fd_, iovs_, count_, 0);
        io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
    }

    int Socket::AsyncReadV::await_resume()
    {
        if (result < 0)
            throw std::system_error(-result, std::system_category(), "AsyncReadV failed");
        return result;
    }

    // --- AsyncWrite ---
    Socket::AsyncWrite::AsyncWrite(int fd, const void *buf, size_t len)
        : fd_(fd), buf_(buf), len_(len) {}

    void Socket::AsyncWrite::await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        auto *ring = core::t_current_worker->ring();
        struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);
        io_uring_prep_send(sqe, fd_, buf_, len_, 0);
        io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
    }

    int Socket::AsyncWrite::await_resume()
    {
        if (result < 0)
            throw std::system_error(-result, std::system_category(), "AsyncWrite failed");
        return result;
    }

    // --- AsyncWriteV ---
    Socket::AsyncWriteV::AsyncWriteV(int fd, std::span<struct iovec> iov_span)
        : fd_(fd), iovs_(iov_span.data()), count_(iov_span.size()) {}

    void Socket::AsyncWriteV::await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        auto *ring = core::t_current_worker->ring();
        struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);
        io_uring_prep_writev(sqe, fd_, iovs_, count_, 0);
        io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
    }

    int Socket::AsyncWriteV::await_resume()
    {
        if (result < 0)
            throw std::system_error(-result, std::system_category(), "AsyncWriteV failed");
        return result;
    }

    // --- AsyncAccept ---
    Socket::AsyncAccept::AsyncAccept(int fd, struct sockaddr *addr, socklen_t *len)
        : server_fd_(fd), client_addr_(addr), client_len_(len) {}

    void Socket::AsyncAccept::await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        auto *ring = core::t_current_worker->ring();
        struct io_uring_sqe *sqe = Socket::get_sqe_safe(ring);
        io_uring_prep_accept(sqe, server_fd_, client_addr_, client_len_, 0);
        io_uring_sqe_set_data(sqe, static_cast<core::BaseAwaiter *>(this));
    }

    int Socket::AsyncAccept::await_resume()
    {
        if (result < 0)
            throw std::system_error(-result, std::system_category(), "AsyncAccept failed");
        return result;
    }
}