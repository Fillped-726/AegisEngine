#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <memory>

// Third-party
#include "concurrentqueue.h"

// Aegis
#include "aegis/core/actor.h"
#include "aegis/common/work_stealing_queue.h" // 刚才那个 Lock-Free Queue 的头文件

using SchedulerTask = aegis::core::Actor *;

namespace aegis::core
{
    class Scheduler
    {
    public:
        static Scheduler &instance();

        void start(int num_workers);
        void stop();

        void dispatch(SchedulerTask task);

    private:
        Scheduler() = default;
        ~Scheduler();

        void worker_entry(int id);
        void notify_one_worker();

        // 这里的返回值适配 WorkStealingQueue::pop/steal 的 optional
        std::optional<SchedulerTask> try_local_pop(int worker_id);
        std::optional<SchedulerTask> try_global_pop(int worker_id);
        std::optional<SchedulerTask> try_steal(int thief_id);

        void execute_actor(SchedulerTask task);

        std::atomic<bool> running_{false};
        std::vector<std::thread> workers_;

        using WorkQueue = aegis::common::WorkStealingQueue<SchedulerTask>;
        std::vector<std::unique_ptr<WorkQueue>> local_queues_;

        // 全局无锁队列
        moodycamel::ConcurrentQueue<SchedulerTask> global_queue_;

        // 线程休眠/唤醒
        std::mutex sleep_mtx_;
        std::condition_variable sleep_cv_;
        std::atomic<int> sleeping_workers_{0};
    };

} // namespace aegis::core