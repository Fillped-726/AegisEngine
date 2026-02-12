#pragma once
#include <liburing.h>
#include <stdexcept>
#include <memory>
#include <mutex> // [Required]
#include "aegis/core/awaiter.h"
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include "concurrentqueue.h"

namespace aegis::net
{
    class Connection;
}

namespace aegis::core
{
    class Env
    {
    public:
        static Env &instance();
        Env(const Env &) = delete;
        Env &operator=(const Env &) = delete;

        void init(int ring_depth = 4096);
        void run();

        io_uring *native_handle() { return &ring_; }

        moodycamel::ConcurrentQueue<net::Connection *> pending_conns_;

        void add_pending_connection(aegis::net::Connection *conn)
        {
            pending_conns_.enqueue(conn);
        }

        void stop()
        {
            is_running_.store(false, std::memory_order_release);
            wake_up(); // 唤醒 io_uring，让它跳出等待循环
        }

        void wake_up()
        {
            uint64_t one = 1;
            // 这是一个 write syscall，但开销很小，且是必须的
            ::write(wakeup_fd_, &one, sizeof(one));
        }

    private:
        Env() = default;
        ~Env();

        void arm_wakeup();
        int wakeup_fd_;
        uint64_t wakeup_buf_;

        struct io_uring ring_;
        bool is_initialized_ = false;
        std::atomic<bool> is_running_{false};
    };
}