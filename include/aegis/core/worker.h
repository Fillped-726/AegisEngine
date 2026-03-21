#pragma once

#include <liburing.h>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <deque>

// Third-party
#include "concurrentqueue.h"

// 假设这些前置声明存在
namespace aegis::core { class Actor; }
namespace aegis::net { class Connection; }

using SchedulerTask = aegis::core::Actor*;

namespace aegis::core
{
    // ===================================================================
    // 核心类：Worker (也即 EventLoop)
    // 每一个 Worker 独占一个系统线程，绑定一个 CPU 核心，拥有独立的 io_uring
    // 绝对禁止跨 Worker 访问非线程安全的数据！
    // ===================================================================
    class Worker
    {
    public:
        Worker(int worker_id);
        ~Worker();

        // 线程的真正入口，死循环
        void run();
        void stop();

        // 提供给其他 Worker 调用的跨核通信接口（无锁投递）
        // 比如 Worker A 想把一个新建的 Connection 扔给 Worker B
        void post_cross_core_task(SchedulerTask task);

        int id() const { return worker_id_; }

    private:
        // --- 核心模块 1：网络 I/O (取代了以前的全局 Env) ---
        void process_io();
        struct io_uring ring_;
        bool is_uring_initialized_ = false;

        // --- 核心模块 2：本地计算任务 ---
        void process_local_tasks();
        // 注意！因为只有当前 Worker 线程会 pop，这里其实可以用更轻量的结构
        // 但为了接收当前线程产生的源源不断的任务，依然保持一个队列
        std::deque<SchedulerTask> local_run_queue_; 

        // --- 核心模块 3：跨核消息接收 (多生产者，单消费者) ---
        void process_cross_core_messages();
        // 其他 Worker 通过 post_cross_core_task 把任务/消息推到这里
        moodycamel::ConcurrentQueue<SchedulerTask> cross_core_queue_;

        int worker_id_;
        std::atomic<bool> is_running_{false};
    };

    // ===================================================================
    // 调度器：退化为一个纯粹的“管理器”和“分发器”
    // 不再负责具体的 while(true) 调度，只负责启动 Worker 和初始哈希路由
    // ===================================================================
    class Scheduler
    {
    public:
        static Scheduler& instance();

        void start(int num_workers);
        void stop();

        // 外部入口：比如 Acceptor 收到一个新连接产生的 Actor，按 Hash 分发给某个 Worker
        void dispatch_to_worker(SchedulerTask task, int target_worker_id);

        // 获取 Worker 实例（主要用于跨核发消息时拿到目标 Worker 的引用）
        Worker* get_worker(int id);

    private:
        Scheduler() = default;
        ~Scheduler() = default;

        std::vector<std::unique_ptr<Worker>> workers_;
        std::vector<std::thread> threads_;
        std::atomic<bool> running_{false};
    };

} // namespace aegis::core