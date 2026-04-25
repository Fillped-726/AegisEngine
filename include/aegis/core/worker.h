/**
 * @file worker.h
 * @brief Per-thread worker with io_uring event loop, coroutine execution, and actor message draining.
 */
#pragma once

#include <liburing.h>
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <deque>
#include <sys/eventfd.h>
#include <unistd.h>
#include "functional"
#include "aegis/core/task.h"
#include "aegis/core/hierarchy_timer.h"

// Third-party
#include "concurrentqueue.h"

// 假设这些前置声明存在
namespace aegis::core
{
    class Actor;
    union ActorID;
}
namespace aegis::net
{
    class Connection;
}

using SchedulerTask = aegis::core::Actor *;

namespace aegis::core
{
    extern thread_local int t_worker_id;
    // ===================================================================
    // Thread-per-Core 的核心引擎：Worker
    // 集 IO 轮询、协程恢复、Actor 状态机调度于一身
    // ===================================================================
    /**
     * @brief Per-thread worker executing the main run loop.
     * 
     * Each worker owns an io_uring instance and alternates between:
     * 1. Polling io_uring completion queue (CQE)
     * 2. Resuming completed coroutines
     * 3. Draining actor message queues assigned to this worker
     */
    class Worker
    {
    public:
        Worker(int worker_id);
        ~Worker();

        // 启动 Event Loop (将被 Scheduler 在新线程中调用)
        void run();

        // 停止当前 Worker
        void stop();

        // [线程安全] 供其他 Worker 调用的跨核通信接口
        void post_cross_core_task(SchedulerTask task);

        // [线程安全] 供其他模块调用的自定义任务接口
        void post_custom_task(MoveOnlyTask task);

        void dispatch_local(SchedulerTask task); // 本地极速派发

        // [线程安全] 唤醒该 Worker 的 io_uring (基于 eventfd)
        void wake_up();

        static int get_current_id()
        {
            return t_worker_id;
        }

        int id() const { return worker_id_; }

        io_uring *ring() { return &ring_; }

        HierarchicalTimeWheel &time_wheel() { return time_wheel_; }

    private:
        // --- 初始化相关 ---
        void init_io_uring();
        void arm_wakeup();

        // --- Event Loop 的三大阶段 ---
        void process_io(bool wait_for_events, uint32_t ms_to_next_tick);
        void process_local_tasks();
        void process_cross_core_messages();

        // --- 从 Scheduler 迁移过来的执行逻辑 ---
        void execute_actor(SchedulerTask task);

        int worker_id_;
        std::atomic<bool> is_running_{false};

        // --- I/O 模块 (原 Env 逻辑) ---
        struct io_uring ring_;
        bool is_uring_initialized_ = false;
        int wakeup_fd_ = -1;
        uint64_t wakeup_buf_ = 0;
        static inline void *const kEventToken = reinterpret_cast<void *>(0xBEEF);

        // --- 定时器模块 (Phase 1 & 2) ---
        HierarchicalTimeWheel time_wheel_;

        // 【面试亮点】：使用标志位合并高频唤醒，避免 eventfd 风暴
        std::atomic<bool> is_waking_up_{false};

        // --- 计算模块 (原 Scheduler 逻辑) ---
        // 本地待执行队列 (当前线程私有，无锁！普通 deque 即可)
        std::deque<SchedulerTask> local_run_queue_;

        // 跨核消息接收队列 (多生产者，单消费者，Lock-free)
        moodycamel::ConcurrentQueue<SchedulerTask> cross_core_queue_;
        std::unique_ptr<moodycamel::ConsumerToken> cross_core_cons_token_;

        // --- 自定义任务队列 (供其他模块投递的非 Actor 任务) ---
        moodycamel::ConcurrentQueue<MoveOnlyTask> custom_task_queue_;
    };

    extern thread_local Worker *t_current_worker;

} // namespace aegis::core