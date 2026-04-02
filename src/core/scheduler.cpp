#include "aegis/core/scheduler.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/actor.h"
#include "aegis/common/tools.h" // 假设里面有 bind_to_core 等工具函数

#ifdef __linux__
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace aegis::core
{
    // 在 Worker.cpp 中定义的 TLS 变量，在这里声明 extern 以便提供访问
    extern thread_local int t_worker_id;

    Scheduler &Scheduler::instance()
    {
        static Scheduler inst;
        return inst;
    }

    Scheduler::~Scheduler() { stop(); }

    void Scheduler::start(int num_workers)
    {
        if (running_)
            return;
        running_ = true;

        aegis::Log::instance().info("Scheduler (Thread-per-Core Mode) starting with {} workers...", num_workers);

        workers_.reserve(num_workers);
        threads_.reserve(num_workers);

        // 1. 先创建所有的 Worker 实例
        for (int i = 0; i < num_workers; ++i)
        {
            workers_.push_back(std::make_unique<Worker>(i));
        }

        // 2. 启动系统线程，拉起 Event Loop
        for (int i = 0; i < num_workers; ++i)
        {
            threads_.emplace_back([this, i]()
                                  {
                // 绑核操作 (如果你之前实现了的话，这里极其关键！)
                // Thread-per-Core 必须严格绑核，防止 OS 调度器把线程切到别的核导致 L1/L2 Cache 全量失效
                bind_to_core(i);

#ifdef __linux__
                std::string name = "Aegis-W-" + std::to_string(i);
                pthread_setname_np(pthread_self(), name.c_str());
#endif
                // 进入死循环，接管当前 CPU 核心
                workers_[i]->run(); });
        }
    }

    void Scheduler::stop()
    {
        if (!running_)
            return;
        running_ = false;

        // 1. 通知所有 Worker 停止循环
        for (auto &worker : workers_)
        {
            if (worker)
                worker->stop();
        }

        // 2. 等待线程安全退出
        for (auto &t : threads_)
        {
            if (t.joinable())
                t.join();
        }

        threads_.clear();
        workers_.clear();

        aegis::Log::instance().info("Scheduler completely stopped.");
    }

    void Scheduler::dispatch(SchedulerTask task)
    {
        if (!task || workers_.empty())
            return;

        // 【负载均衡】：简单的 Round-Robin 算法
        // 由于没有了全局队列，新建的 Actor 必须在这里决定它的终身归宿
        uint64_t idx = rr_counter_.fetch_add(1, std::memory_order_relaxed);
        int target_worker_id = idx % workers_.size();

        // 投递给目标 Worker 的私有跨核队列
        workers_[target_worker_id]->post_cross_core_task(task);
    }

    int Scheduler::current_worker_id()
    {
        return t_worker_id;
    }

    Worker *Scheduler::get_worker(int id)
    {
        if (id >= 0 && id < static_cast<int>(workers_.size()))
        {
            return workers_[id].get();
        }
        return nullptr;
    }

} // namespace aegis::core