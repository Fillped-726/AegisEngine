/**
 * @file scheduler.h
 * @brief Worker pool manager. Creates N workers, each with its own io_uring instance.
 */
#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <memory>

// Aegis
#include "aegis/core/worker.h"

// 前置声明
namespace aegis::core
{
    class Actor;
}
using SchedulerTask = aegis::core::Actor *;

namespace aegis::core
{
    // ===================================================================
    // Thread-per-Core 架构下的 Scheduler (控制面)
    // 仅负责 Worker 的生命周期管理与初始负载均衡，不再参与具体调度
    // ===================================================================
    /**
     * @brief Worker pool manager.
     * Creates N workers (configurable), each with its own io_uring instance.
     * Workers are started and joined by the scheduler.
     */
    class Scheduler
    {
    public:
        static Scheduler &instance();

        // 启动指定数量的 Worker 线程 (通常 = CPU 核心数)
        void start(int num_workers);

        // 安全停止所有 Worker
        void stop();

        // 【核心入口】将一个全新的 Actor 或任务分配给某个 Worker
        // 采用 Round-Robin (轮询) 策略实现基础负载均衡
        void dispatch(SchedulerTask task);

        // 获取当前线程所在的 Worker ID
        static int current_worker_id();

        // 获取特定 Worker 的指针 (用于跨核通信时的精确投递)
        Worker *get_worker(int id);

    private:
        Scheduler() = default;
        ~Scheduler();

        std::atomic<bool> running_{false};

        // Worker 实例数组 (生命周期与 Scheduler 绑定)
        std::vector<std::unique_ptr<Worker>> workers_;

        // 实际执行的系统线程
        std::vector<std::thread> threads_;

        // 用于 Round-Robin 分发的原子计数器
        std::atomic<uint64_t> rr_counter_{0};
    };

} // namespace aegis::core