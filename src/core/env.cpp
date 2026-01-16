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

        struct io_uring_params params;
        memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;

        if (io_uring_queue_init_params(ring_depth, &ring_, &params) < 0)
        {
            int err = errno;
            aegis::Log::instance().critical("Failed to init io_uring with SQPOLL. Errno: {}", err);
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

            int ret = io_uring_wait_cqe(&ring_, &cqe);

            if (ret < 0)
            {
                if (ret == -EINTR)
                    continue;
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