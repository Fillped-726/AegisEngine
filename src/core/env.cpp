#include "aegis/core/env.h"
#include <coroutine>

namespace aegis::core
{

    // 定义一个基类 Awaiter 用于找回协程句柄
    // 这部分 trick 是为了让 Env::run 能通用处理 Accept/Read/Write
    struct BaseAwaiter
    {
        int result = 0;
        std::coroutine_handle<> handle;
    };

    Env &Env::instance()
    {
        static Env instance;
        return instance;
    }

    void Env::init(int ring_depth)
    {
        if (is_initialized_)
            return;
        if (io_uring_queue_init(ring_depth, &ring_, 0) < 0)
        {
            throw std::runtime_error("Failed to init io_uring");
        }
        is_initialized_ = true;
    }

    Env::~Env()
    {
        if (is_initialized_)
        {
            io_uring_queue_exit(&ring_);
        }
    }

    void Env::run()
    {
        while (true)
        {
            // 提交并等待至少 1 个完成事件
            io_uring_submit_and_wait(&ring_, 1);

            struct io_uring_cqe *cqe;
            unsigned head;
            unsigned count = 0;

            // 遍历完成队列 (Completion Queue)
            io_uring_for_each_cqe(&ring_, head, cqe)
            {
                // 取出之前存入的 Awaiter 指针
                auto *awaiter = reinterpret_cast<BaseAwaiter *>(io_uring_cqe_get_data(cqe));

                if (awaiter)
                {
                    // 1. 设置结果 (bytes read 或 error code)
                    awaiter->result = cqe->res;

                    // 2. 唤醒协程
                    if (awaiter->handle)
                    {
                        awaiter->handle.resume();
                    }
                }

                count++;
            }
            // 标记这些 cqe 已处理
            io_uring_cq_advance(&ring_, count);
        }
    }

} // namespace aegis::core