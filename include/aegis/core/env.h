#pragma once
#include <liburing.h>
#include <stdexcept>
#include <memory>
#include <mutex> // [Required]
#include "aegis/core/awaiter.h"

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

        // [Fix] 恢复这两个接口供 Socket 使用
        io_uring *native_handle() { return &ring_; }
        std::mutex &get_submission_mutex() { return sq_mutex_; }

    private:
        Env() = default;
        ~Env();

        struct io_uring ring_;
        bool is_initialized_ = false;
        std::mutex sq_mutex_; // [Required] 保护 SQ
    };
}