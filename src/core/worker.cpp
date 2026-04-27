#include "aegis/core/worker.h"
#include "aegis/core/actor.h"
#include "aegis/core/actor_registry.h"
#include "aegis/core/awaiter.h" // 假设存在
#include "aegis/common/actor_utils.h"
#include "aegis/common/aegisLog.h"
#include "aegis/common/time_utils.h"
#include <cstring>
#include <stdexcept>

using TimeUtil = aegis::common::TimeUtil;

namespace aegis::core
{
    // TLS 变量，绑定当前线程对应的 Worker ID
    thread_local Worker *t_current_worker = nullptr;
    thread_local int t_worker_id = -1;

    Worker::Worker(int worker_id) : worker_id_(worker_id)
    {
        cross_core_cons_token_ = std::make_unique<moodycamel::ConsumerToken>(cross_core_queue_);
    }

    Worker::~Worker()
    {
        stop();
        if (is_uring_initialized_)
        {
            io_uring_queue_exit(&ring_);
            if (wakeup_fd_ >= 0)
                close(wakeup_fd_);
        }
    }

    void Worker::init_io_uring()
    {
        // 1. 创建私有 eventfd，用于接收跨核事件唤醒
        wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0)
        {
            throw std::runtime_error("Worker failed to create eventfd");
        }

        // 2. 初始化 io_uring
        // 【注意】：在 Thread-per-Core 下，我们通常不再使用 SQPOLL！
        // 因为 Worker 线程本身就是个永不停歇的轮询器，开 SQPOLL 会多出一个内核线程，
        // 导致 1核2线程 的资源浪费，这违背了 Share-Nothing 的极致亲和性原则。
        struct io_uring_params params;
        memset(&params, 0, sizeof(params));
        // 取消 IORING_SETUP_SQPOLL，回归普通模式，由当前 Worker 自己 submit

        int ring_depth = 4096;
        if (io_uring_queue_init_params(ring_depth, &ring_, &params) < 0)
        {
            close(wakeup_fd_);
            throw std::runtime_error("Worker failed to init io_uring");
        }

        is_uring_initialized_ = true;
        arm_wakeup();
        io_uring_submit(&ring_); // 提交第一次的门铃监听
    }

    void Worker::arm_wakeup()
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
            return; // 真实环境需要处理 SQ 满的情况
        io_uring_prep_read(sqe, wakeup_fd_, &wakeup_buf_, sizeof(wakeup_buf_), 0);
        io_uring_sqe_set_data(sqe, kEventToken);
    }

    void Worker::wake_up()
    {
        // 优化：只有在没有正在唤醒的情况下，才去发起 syscall
        bool expected = false;
        if (is_waking_up_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            Log::instance().debug("Worker {} is being kicked!", worker_id_);
            uint64_t one = 1;
            ::write(wakeup_fd_, &one, sizeof(one));
        }
    }

    void Worker::post_cross_core_task(SchedulerTask task)
    {
        // 其他线程调用的接口
        cross_core_queue_.enqueue(task);
        wake_up(); // 敲响目标 Worker 的门铃
    }

    void Worker::dispatch_local(SchedulerTask task)
    {
        // 【极速派发】：毫无竞争，直接塞入本地双端队列尾部
        local_run_queue_.push_back(task);
    }

    void Worker::run()
    {
        t_worker_id = worker_id_;
        t_current_worker = this;
        init_io_uring();

        aegis::Log::instance().info("Worker {} EventLoop started.", worker_id_);
        is_running_.store(true, std::memory_order_relaxed);

        uint64_t last_tick_time_ms = TimeUtil::get_now_ms();

        while (is_running_.load(std::memory_order_acquire))
        {
            // 阶段 1：处理跨核消息 (优先级最高，里面可能有新的 Actor/Connection 转移过来)
            process_cross_core_messages();

            // 阶段 2：处理本地 Actor 计算
            process_local_tasks();

            uint64_t now_ms = TimeUtil::get_now_ms();

            while (now_ms >= last_tick_time_ms + TICK_MS)
            {
                time_wheel_.tick();           // 拨动齿轮，触发定时回调
                last_tick_time_ms += TICK_MS; // 严格按照 20ms 对齐推进
            }

            // 阶段 3：处理网络 I/O
            bool wait_for_events = local_run_queue_.empty();
            uint32_t ms_to_next_tick = (last_tick_time_ms + TICK_MS) - TimeUtil::get_now_ms();
            if (ms_to_next_tick > TICK_MS)
                ms_to_next_tick = 0;
            process_io(wait_for_events, ms_to_next_tick);
        }
    }

    void Worker::process_cross_core_messages()
    {
        // 1. 消费 Actor 任务 (原有的)
        SchedulerTask task;
        while (cross_core_queue_.try_dequeue(*cross_core_cons_token_, task))
        {
            dispatch_local(task);
        }

        // 2. 消费通用闭包任务
        MoveOnlyTask custom_task;
        while (custom_task_queue_.try_dequeue(custom_task))
        {
            custom_task();
        }
    }

    void Worker::process_local_tasks()
    {
        // 为了防止 Actor 饥饿，我们每一轮只处理当前快照数量的任务，或者限制固定 Batch
        size_t task_count = local_run_queue_.size();
        for (size_t i = 0; i < task_count; ++i)
        {
            SchedulerTask task = std::move(local_run_queue_.front());
            local_run_queue_.pop_front();

            execute_actor(task); // 执行该 Actor 的状态机
        }
    }

    void Worker::process_io(bool wait_for_events, uint32_t ms_to_next_tick)
    {
        struct io_uring_cqe *cqe;

        if (wait_for_events)
        {
            // 修正：将计算出的 ms_to_next_tick 转换为 io_uring 的超时结构
            struct __kernel_timespec ts;
            ts.tv_sec = ms_to_next_tick / 1000;
            ts.tv_nsec = (ms_to_next_tick % 1000) * 1000000;

            // 使用支持超时的等待接口，确保定时器不会饥饿
            // 注意：这里需要 liburing 较新版本的支持
            io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &ts, nullptr);
        }
        else
        {
            io_uring_submit(&ring_);
        }

        // 消费 CQE (这一段与原 Env 基本一致)
        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            void *user_data = io_uring_cqe_get_data(cqe);

            if (user_data == kEventToken)
            {
                // 门铃响了，清除唤醒标志，重新监听
                is_waking_up_.store(false, std::memory_order_release);
                arm_wakeup();
            }
            else if (user_data)
            {
                // 处理真实的 IO (恢复协程等)
                auto *awaiter = reinterpret_cast<BaseAwaiter *>(user_data);
                awaiter->result = cqe->res;
                if (awaiter->handle)
                {
                    try
                    {
                        awaiter->handle.resume();
                    }
                    catch (const std::exception &e)
                    { /* Handle IO Coroutine Error */
                        aegis::Log::instance().error("IO Coroutine Error: {}", e.what());
                    }
                }
            }
        }
        if (count > 0)
        {
            io_uring_cq_advance(&ring_, count);
        }
    }

    // ==========================================================
    // 从 Scheduler 1:1 迁移过来的 Actor 状态机流转核心逻辑
    // ==========================================================
    void Worker::execute_actor(SchedulerTask actor)
    {
        Actor::set_current(actor);
        ActorState state = ActorState::Active;
        int death_reason = 0;

        try
        {
            state = actor->process_batch(100);
        }
        catch (const std::exception &e)
        {
            aegis::Log::instance().error("Actor {} crashed: {}", actor->id().raw, e.what());
            state = ActorState::Dead;
            death_reason = 1;
        }
        catch (...)
        {
            aegis::Log::instance().error("Actor {} crashed with unknown exception", actor->id().raw);
            state = ActorState::Dead;
            death_reason = 1;
        }

        Actor::set_current(nullptr);

        switch (state)
        {
        case ActorState::Active:
            // 继续活跃，直接用无锁接口放回本地队列末尾
            dispatch_local(actor);
            break;

        case ActorState::Idle:
            // 挂起，等待其他 Actor push 消息或 IO 唤醒
            break;

        case ActorState::Dead:
            // --- 死亡逻辑不变，直接复用你之前的完美设计 ---
            ActorID my_id = actor->id();
            ActorID supervisor_id = actor->parent_id();
            ActorRegistry::instance().remove(my_id);

            if (supervisor_id.raw != 0)
            {
                Actor *supervisor = ActorRegistry::instance().get(supervisor_id);
                if (supervisor)
                {
                    auto *msg = new ActorDiedMsg(my_id, death_reason);
                    dispatch_msg(supervisor, msg);
                }
            }
            actor->finalize();
            break;
        }
    }

    void Worker::stop()
    {
        is_running_.store(false, std::memory_order_release);
        wake_up(); // 防止卡在 io_uring_submit_and_wait 里
    }

    void Worker::post_custom_task(MoveOnlyTask task)
    {
        custom_task_queue_.enqueue(std::move(task));
        wake_up(); // 敲响目标核的门铃
    }

    int aegis::core::ActorRegistry::get_current_worker_id()
    {
        return (t_current_worker != nullptr) ? t_current_worker->id() : -1;
    }

} // namespace aegis::core