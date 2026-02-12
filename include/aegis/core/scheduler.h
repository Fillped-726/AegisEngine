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
#include "aegis/core/actor_traits.h"
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

        void monitor_entry();

        // 这里的返回值适配 WorkStealingQueue::pop/steal 的 optional
        std::optional<SchedulerTask> try_local_pop(int worker_id);
        std::optional<SchedulerTask> try_global_pop(int worker_id);
        std::optional<SchedulerTask> try_steal(int thief_id);

        void execute_actor(SchedulerTask task);

        void handle_actor_death(Actor *actor, int reason);
        void cleanup_graveyard();

        static int64_t now_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

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

        // --- 延迟回收数据结构 ---

        // 1. 坟墓队列 (Lock-Free)
        // 多生产者(Workers) -> 单消费者(Monitor)
        moodycamel::ConcurrentQueue<Actor *> graveyard_queue_;

        // 2. 本地待销毁列表 (Thread-Local to Monitor Thread)
        // 结构: {死亡时间戳, Actor指针}
        struct DeadActorEntry
        {
            int64_t death_time;
            Actor *actor;
        };
        // 使用 deque 因为我们需要高效的头部删除
        std::deque<DeadActorEntry> pending_deletions_;

        // 3. 安全时间窗口 (配置项)
        // 5000ms 足够覆盖任何极端情况下的 CPU 调度延迟
        static constexpr int64_t GRAVEYARD_DELAY_MS = 5000;
    };

} // namespace aegis::core