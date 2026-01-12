#include "aegis/core/env.h"
#include "aegis/common/aegisLog.h"
#include <coroutine>
#include <thread>
#include <chrono>

namespace aegis::core
{

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
            aegis::Log::instance().critical("Failed to init io_uring.");
            throw std::runtime_error("Failed to init io_uring");
        }
        is_initialized_ = true;
        aegis::Log::instance().info("Env (IO_Uring) initialized. Depth: {}", ring_depth);
    }

    Env::~Env()
    {
        if (is_initialized_)
        {
            io_uring_queue_exit(&ring_);
            aegis::Log::instance().info("Env (IO_Uring) exited.");
        }
    }

    void Env::run()
    {
        aegis::Log::instance().info("Env EventLoop started running (Pure Wait Mode).");

        while (true)
        {
            struct io_uring_cqe *cqe;

            // [Critical Fix] 使用 io_uring_wait_cqe 替代 io_uring_submit_and_wait
            // 原因：submit_and_wait 会尝试访问 Submission Queue (SQ) 来提交挂起的请求，
            // 但此时 Worker 线程可能正持有锁在操作 SQ，导致无锁竞争和 Segfault。
            // 既然 Workers 已经显式调用了 io_uring_submit，这里只需要傻等结果 (CQ) 即可。
            int ret = io_uring_wait_cqe(&ring_, &cqe);

            if (ret < 0)
            {
                if (ret == -EINTR)
                    continue; // 信号中断
                aegis::Log::instance().error("io_uring_wait_cqe error: {}", -ret);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // 既然 wait_cqe 返回了，说明至少有一个 CQE 就绪
            // 使用 for_each_cqe 批量处理所有就绪的事件，提高吞吐量
            unsigned head;
            unsigned count = 0;

            io_uring_for_each_cqe(&ring_, head, cqe)
            {
                auto *awaiter = reinterpret_cast<BaseAwaiter *>(io_uring_cqe_get_data(cqe));

                if (awaiter)
                {
                    awaiter->result = cqe->res;
                    if (awaiter->handle)
                    {
                        try
                        {
                            awaiter->handle.resume();
                        }
                        catch (const std::exception &e)
                        {
                            aegis::Log::instance().error("Coroutine resume exception: {}", e.what());
                        }
                        catch (...)
                        {
                            aegis::Log::instance().error("Unknown exception in coroutine.");
                        }
                    }
                }
                count++;
            }

            // 批量标记这些 CQE 已处理，归还槽位给内核
            io_uring_cq_advance(&ring_, count);
        }
    }

} // namespace aegis::core