#include "aegis/core/env.h"
#include "aegis/common/aegisLog.h"
#include "aegis/net/connection.h" // 必须包含，以便调用 conn->flush()
#include <coroutine>
#include <thread>
#include <chrono>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>

namespace aegis::core
{
    // 特殊标记，用于识别 CQE 是否为 eventfd 唤醒事件
    static void *kEventToken = (void *)0xBEEF;

    Env &Env::instance()
    {
        static Env instance;
        return instance;
    }

    void Env::init(int ring_depth)
    {
        if (is_initialized_)
            return;

        // 1. 创建 eventfd (非阻塞模式)
        // 这是 Worker 线程唤醒 IO 线程的"门铃"
        wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0)
        {
            int err = errno;
            aegis::Log::instance().critical("Failed to create eventfd. Errno: {}", err);
            throw std::runtime_error("Failed to create eventfd");
        }

        struct io_uring_params params;
        memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_SQPOLL; // 保持 SQPOLL 模式
        params.sq_thread_idle = 2000;

        if (io_uring_queue_init_params(ring_depth, &ring_, &params) < 0)
        {
            int err = errno;
            aegis::Log::instance().critical("Failed to init io_uring with SQPOLL. Errno: {}", err);
            close(wakeup_fd_);
            throw std::runtime_error("Failed to init io_uring");
        }

        is_initialized_ = true;

        // 2. 挂载第一个唤醒监听
        arm_wakeup();

        // 3. 立即提交，确保内核开始监听 eventfd
        io_uring_submit(&ring_);

        aegis::Log::instance().info("Env initialized. Depth: {}, EventFD: {}", ring_depth, wakeup_fd_);
    }

    Env::~Env()
    {
        if (is_initialized_)
        {
            io_uring_queue_exit(&ring_);
            if (wakeup_fd_ >= 0)
                close(wakeup_fd_);
            aegis::Log::instance().info("Env exited.");
        }
    }

    // 辅助函数：重新挂载 eventfd 读请求
    void Env::arm_wakeup()
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
        {
            // 极罕见情况：SQ 满了。
            // 简单策略：尝试提交一次腾空间，或者记录日志。
            // 在 SQPOLL 下通常内核会很快消费。
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe)
            {
                aegis::Log::instance().error("SQ full when arming wakeup!");
                return;
            }
        }

        // 准备读取 eventfd
        io_uring_prep_read(sqe, wakeup_fd_, &wakeup_buf_, sizeof(wakeup_buf_), 0);
        // 【关键】设置特殊 UserData
        io_uring_sqe_set_data(sqe, kEventToken);
    }

    void Env::run()
    {
        aegis::Log::instance().info("Env EventLoop started running (Proactor Mode).");

        is_running_.store(true, std::memory_order_relaxed);

        while (is_running_.load(std::memory_order_acquire))
        {
            // --- Phase 1: Flush (搬运工) ---
            // 检查全局就绪队列，将 Worker 产生的 Outbox 数据转为 Write SQE
            net::Connection *conn = nullptr;
            // 假设你已经在 Env.h 里定义了 moodycamel::ConcurrentQueue<net::Connection*> pending_conns_;
            while (pending_conns_.try_dequeue(conn))
            {
                if (conn)
                {
                    conn->flush(); // 产生 Write SQE (填入 Ring，未提交)
                }
            }

            // --- Phase 2: Submit & Wait (发射 & 等待) ---
            // 这里的 submit 会把 Phase 1 产生的 Write 请求，以及之前 Read 产生的请求，
            // 还有 arm_wakeup 产生的监听请求，一次性发给内核。
            struct io_uring_cqe *cqe;
            int ret = io_uring_submit_and_wait(&ring_, 1);

            if (ret < 0)
            {
                if (ret == -EINTR)
                    continue;
                aegis::Log::instance().error("io_uring_wait_cqe error: {}", -ret);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // --- Phase 3: Process Completion (回调) ---
            unsigned head;
            unsigned count = 0;

            io_uring_for_each_cqe(&ring_, head, cqe)
            {
                count++;
                void *user_data = io_uring_cqe_get_data(cqe);

                // Case A: 门铃响了 (Worker 唤醒我们处理发包)
                if (user_data == kEventToken)
                {
                    // 只要读到了，就说明 Worker 刚才按了门铃
                    // 我们已经在 Phase 1 处理了队列（或者下一轮 Phase 1 会处理）
                    // 所以这里只需要重新挂载监听即可
                    arm_wakeup();
                }
                // Case B: 普通 IO 完成 (Read/Write)
                else if (user_data)
                {
                    // 这里可能是 Awaiter，也可能是 WriteContext (取决于 Connection::flush 的实现)
                    // 假设目前只有 Awaiter (Read)
                    auto *awaiter = reinterpret_cast<BaseAwaiter *>(user_data);

                    awaiter->result = cqe->res;
                    if (awaiter->handle)
                    {
                        try
                        {
                            awaiter->handle.resume();
                        }
                        catch (const std::exception &e)
                        {
                            aegis::Log::instance().error("Coroutine exception: {}", e.what());
                        }
                        catch (...)
                        {
                            aegis::Log::instance().error("Unknown exception.");
                        }
                    }
                }
            }

            // 归还 CQE 槽位
            io_uring_cq_advance(&ring_, count);
        }
    }

} // namespace aegis::core