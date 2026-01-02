#pragma once
#include <liburing.h>
#include <stdexcept>
#include <memory>
#include "aegis/core/awaiter.h"

namespace aegis::core
{

    class Env
    {
    public:
        // 获取单例
        static Env &instance();

        Env(const Env &) = delete;
        Env &operator=(const Env &) = delete;

        // 初始化 io_uring
        void init(int ring_depth = 4096);

        // 启动事件循环
        void run();

        // 获取原始 ring 指针 (给 Socket 用)
        io_uring *get_ring() { return &ring_; }

    private:
        Env() = default;
        ~Env();

        struct io_uring ring_;
        bool is_initialized_ = false;
    };

} // namespace aegis::core