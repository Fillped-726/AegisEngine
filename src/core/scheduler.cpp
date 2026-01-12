#include "aegis/core/scheduler.h"
#include "aegis/common/aegisLog.h"
#include <random>

// 平台兼容性处理：线程命名
#ifdef __linux__
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace aegis::core
{

    // --- 线程本地上下文 (Thread Local Context) ---
    // 加速 Worker 身份识别与队列访问
    static thread_local int t_worker_id = -1;
    static thread_local std::unique_ptr<moodycamel::ProducerToken> t_prod_token;
    static thread_local std::unique_ptr<moodycamel::ConsumerToken> t_cons_token;

    Scheduler &Scheduler::instance()
    {
        static Scheduler inst;
        return inst;
    }

    Scheduler::~Scheduler()
    {
        stop();
    }

    void Scheduler::start(int num_workers)
    {
        if (running_)
        {
            aegis::Log::instance().warn("Scheduler is already running.");
            return;
        }

        running_ = true;

        // 1. 初始化本地队列资源
        local_queues_.resize(num_workers);
        for (int i = 0; i < num_workers; ++i)
        {
            local_queues_[i] = std::make_unique<LocalQueue>();
        }

        aegis::Log::instance().info("Scheduler engine starting with {} workers (Work-Stealing)...", num_workers);

        // 2. 启动 Worker 线程
        workers_.reserve(num_workers);
        for (int i = 0; i < num_workers; ++i)
        {
            workers_.emplace_back([this, i]
                                  { this->worker_entry(i); });
        }
    }

    void Scheduler::stop()
    {
        if (!running_)
            return;

        aegis::Log::instance().info("Scheduler stopping...");
        running_ = false;

        // 唤醒所有正在休眠的 Worker，让它们退出循环
        sleep_cv_.notify_all();

        for (auto &t : workers_)
        {
            if (t.joinable())
                t.join();
        }
        workers_.clear();
        local_queues_.clear(); // 清理本地队列资源

        aegis::Log::instance().info("Scheduler engine stopped.");
    }

    // 核心调度接口实现
    void Scheduler::dispatch(SchedulerTask task)
    {
        if (!task)
            return;

        // Path A: Worker 内部产生的任务 (例如 Actor 处理完还有剩余，或者 Actor 发给另一个 Actor)
        // 策略：放入本地队列 (LIFO) -> 极大提升 Cache 命中率
        if (t_worker_id >= 0)
        {
            auto &lq = local_queues_[t_worker_id];

            lq->lock.lock();
            // 使用 move 避免 shared_ptr 引用计数无谓增加
            lq->q.push(std::move(task));
            lq->lock.unlock();
            return;
        }

        // Path B: IO 线程/外部产生的任务
        // 策略：放入全局队列 (FIFO) -> 无锁，高吞吐
        if (!t_prod_token)
        {
            t_prod_token = std::make_unique<moodycamel::ProducerToken>(global_queue_);
        }

        // 使用 move 传递
        global_queue_.enqueue(*t_prod_token, std::move(task));

        // 关键：如果有任务进入全局队列，必须唤醒一个睡觉的 Worker
        notify_one_worker();
    }

    void Scheduler::notify_one_worker()
    {
        // 性能优化：只有当检测到有 Worker 在睡觉时才去拿锁唤醒
        if (sleeping_workers_.load(std::memory_order_relaxed) > 0)
        {
            std::lock_guard<std::mutex> lock(sleep_mtx_);
            sleep_cv_.notify_one();
        }
    }

    void Scheduler::worker_entry(int id)
    {
        t_worker_id = id;
        // 初始化 Moodycamel 队列的 Token，消除全局锁竞争
        t_prod_token = std::make_unique<moodycamel::ProducerToken>(global_queue_);
        t_cons_token = std::make_unique<moodycamel::ConsumerToken>(global_queue_);

        // 设置线程名，方便调试
#ifdef __linux__
        std::string name = "Aegis-W-" + std::to_string(id);
        pthread_setname_np(pthread_self(), name.c_str());
#endif

        aegis::Log::instance().debug("Worker [{}] initialized.", id);

        // 自适应自旋参数
        const int MAX_SPINS = 4000;
        int spin_count = 0;

        while (running_)
        {
            SchedulerTask task = nullptr;

            // Priority 1: Local LIFO (Hot Cache, Fast)
            if (auto t = try_local_pop(id))
                task = std::move(*t);
            // Priority 2: Global FIFO (Fairness, Batched)
            else if (auto t = try_global_pop(id))
                task = std::move(*t);
            // Priority 3: Steal from others (Load Balance)
            else if (auto t = try_steal(id))
                task = std::move(*t);

            if (task)
            {
                spin_count = 0; // 重置自旋计数
                execute_actor(std::move(task));
                continue;
            }

            // --- Idle Strategy (空闲策略) ---

            // 阶段 1: 忙等 (Busy Wait)
            // 预期很快会有任务到来，不让出 CPU，保持流水线热度
            if (spin_count < MAX_SPINS)
            {
                spin_count++;
                _mm_pause(); // Intel/AMD: Pause instruction
                continue;
            }

            // 阶段 2: 挂起 (Parking)
            // 自旋失败，说明系统真的闲下来了，让出 CPU 节能
            sleeping_workers_.fetch_add(1);
            {
                std::unique_lock<std::mutex> lock(sleep_mtx_);

                if (!running_)
                    break;

                // 使用 wait_for 而不是 wait，防止永久阻塞（兜底机制）
                // 谓词：停止运行 或 全局队列有货
                sleep_cv_.wait_for(lock, std::chrono::milliseconds(1), [this]
                                   { return !running_ || global_queue_.size_approx() > 0; });
            }
            sleeping_workers_.fetch_sub(1);

            // 醒来后，重置自旋，给予它再次进入热路径的机会
            spin_count = 0;
        }
    }

    // 辅助函数：执行 Actor 逻辑
    void Scheduler::execute_actor(SchedulerTask task)
    {
        // [Safety Guard needed in Actor]
        // 这里假设 process_batch 内部是异常安全的。
        // 如果 process_batch 抛出异常，整个 Worker 线程会挂掉。
        // 建议在 Actor::process_batch 内部进行 catch。

        // process_batch 返回 true 表示还有未处理完的消息 (Quota 耗尽)
        bool has_more = task->process_batch(100);

        if (has_more)
        {
            // 如果没处理完，立刻重新调度
            // dispatch 会自动把它放回本地队列队尾 (LIFO)，
            // 这意味着它会马上再次被执行 (Run-to-Completion 近似效果)
            dispatch(std::move(task));
        }
        // 否则 Actor 状态已重置，引用计数归零（如果没被其他地方引用），等待下次 IO 唤醒
    }

    // 实现：尝试从本地队列获取 (LIFO)
    std::optional<SchedulerTask> Scheduler::try_local_pop(int id)
    {
        auto &lq = local_queues_[id];

        // 快速无锁检查
        if (lq->q.empty())
            return std::nullopt;

        lq->lock.lock();
        // Double check inside lock
        if (lq->q.empty())
        {
            lq->lock.unlock();
            return std::nullopt;
        }

        SchedulerTask t = lq->q.pop();
        lq->lock.unlock();

        // 队列可能返回 nullptr (逻辑上的空)
        if (t)
            return t;
        return std::nullopt;
    }

    // 实现：尝试从全局队列获取 (Batching)
    std::optional<SchedulerTask> Scheduler::try_global_pop(int id)
    {
        SchedulerTask t;

        // 批量搬运策略：一次性搬运最多 BATCH_SIZE 个到本地
        constexpr int BATCH_SIZE = 16;
        SchedulerTask buffer[BATCH_SIZE];

        // moodycamel::try_dequeue_bulk 是高效的批量操作
        size_t count = global_queue_.try_dequeue_bulk(*t_cons_token, buffer, BATCH_SIZE);

        if (count == 0)
            return std::nullopt;

        // 第一个任务：立即返回去执行
        t = std::move(buffer[0]);

        // 剩余任务 (1..count-1)：存入本地队列
        if (count > 1)
        {
            auto &lq = local_queues_[id];
            lq->lock.lock();
            for (size_t i = 1; i < count; ++i)
            {
                lq->q.push(std::move(buffer[i]));
            }
            lq->lock.unlock();
        }

        return t;
    }

    // 实现：尝试从其他 Worker 窃取 (Stealing)
    std::optional<SchedulerTask> Scheduler::try_steal(int thief_id)
    {
        int num_workers = workers_.size();
        if (num_workers <= 1)
            return std::nullopt;

        // 随机选择受害者
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, num_workers - 1);

        int start_idx = dist(rng);

        // 尝试探测最多 3 个受害者
        for (int i = 0; i < 3; ++i)
        {
            int victim_id = (start_idx + i) % num_workers;

            if (victim_id == thief_id)
                continue;

            auto &victim_q = local_queues_[victim_id];

            // 关键：Steal 使用 try_lock，抢不到就走，绝不等待
            if (victim_q->lock.try_lock())
            {
                SchedulerTask t = victim_q->q.steal();
                victim_q->lock.unlock();

                if (t)
                {
                    // aegis::Log::instance().debug("Worker {} stole from {}", thief_id, victim_id);
                    return t;
                }
            }
        }

        return std::nullopt;
    }

} // namespace aegis::core