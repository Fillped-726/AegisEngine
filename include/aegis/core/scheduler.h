#pragma once

#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <array>
#include <memory>

// Third-party
#include "concurrentqueue.h"

// Aegis
#include "aegis/core/actor.h"
#include "aegis/common/spinLock.h"
#include "aegis/common/work_stealing_queue.h"

// 定义任务类型
using SchedulerTask = std::shared_ptr<aegis::core::Actor>;

namespace aegis::core
{
    class Scheduler
    {
    public:
        static Scheduler &instance();

        void start(int num_workers);
        void stop();

        /// @brief 核心调度接口
        /// @param task 待执行的 Actor
        /// - Worker 调用：放入本地 LIFO
        /// - 外部调用：放入全局 FIFO
        void dispatch(SchedulerTask task);

    private:
        Scheduler() = default;
        ~Scheduler();

        void worker_entry(int id);
        void notify_one_worker();

        std::optional<SchedulerTask> try_local_pop(int worker_id);
        std::optional<SchedulerTask> try_global_pop(int worker_id);
        std::optional<SchedulerTask> try_steal(int thief_id);

        void execute_actor(SchedulerTask task);

        struct LocalQueue
        {
            common::WorkStealingQueue<SchedulerTask> q;
            common::SpinLock lock;
        };

        std::atomic<bool> running_{false};
        std::vector<std::thread> workers_;

        // 本地队列集合
        std::vector<std::unique_ptr<LocalQueue>> local_queues_;

        // 全局无锁队列 (MPMC)
        moodycamel::ConcurrentQueue<SchedulerTask> global_queue_;

        // 线程休眠/唤醒控制
        std::mutex sleep_mtx_;
        std::condition_variable sleep_cv_;
        std::atomic<int> sleeping_workers_{0};
        std::atomic<long> global_task_count_{0};
    };

} // namespace aegis::core