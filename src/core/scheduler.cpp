#include "aegis/core/scheduler.h"
#include "aegis/common/aegisLog.h"

// 平台兼容性处理：线程命名
#ifdef __linux__
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace aegis::core
{

    // --- 线程本地上下文 (Thread Local Context) ---
    static thread_local int t_worker_id = -1;
    static thread_local std::unique_ptr<moodycamel::ProducerToken> t_prod_token;
    static thread_local std::unique_ptr<moodycamel::ConsumerToken> t_cons_token;

    // [Fix A] 轻量级伪随机数生成器 (Xorshift32)
    // 状态仅 4 字节，无需初始化 heavy 的 mt19937 状态向量
    static thread_local uint32_t t_rng_seed = 0;

    // 简单的种子初始化混淆函数
    inline uint32_t init_seed()
    {
        uint64_t t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return static_cast<uint32_t>(t ^ (t >> 32));
    }

    // 极速随机数：用于负载均衡牺牲者选择
    inline uint32_t fast_rand()
    {
        if (t_rng_seed == 0) [[unlikely]]
        {
            t_rng_seed = init_seed();
            if (t_rng_seed == 0)
                t_rng_seed = 0xDEADBEEF; // 保证非零
        }
        // Xorshift32 算法
        uint32_t x = t_rng_seed;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        t_rng_seed = x;
        return x;
    }

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
        // [Fix B] 重置计数器
        global_task_count_.store(0);

        local_queues_.resize(num_workers);
        for (int i = 0; i < num_workers; ++i)
        {
            local_queues_[i] = std::make_unique<LocalQueue>();
        }

        aegis::Log::instance().info("Scheduler engine starting with {} workers (Work-Stealing)...", num_workers);

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

        sleep_cv_.notify_all();

        for (auto &t : workers_)
        {
            if (t.joinable())
                t.join();
        }
        workers_.clear();
        local_queues_.clear();

        aegis::Log::instance().info("Scheduler engine stopped.");
    }

    void Scheduler::dispatch(SchedulerTask task)
    {
        if (!task)
            return;

        // Path A: 本地任务 (Worker 内部产生)
        if (t_worker_id >= 0)
        {
            auto &lq = local_queues_[t_worker_id];
            lq->lock.lock();
            lq->q.push(std::move(task));
            lq->lock.unlock();
            return;
        }

        // Path B: 外部/IO 任务 -> 全局队列
        if (!t_prod_token)
        {
            t_prod_token = std::make_unique<moodycamel::ProducerToken>(global_queue_);
        }

        // [Fix B] 先增加计数，再 Enqueue，确保 Worker 醒来时一定能看到 count > 0
        // 虽然有极短暂的时间窗口 count > queue.size，但这只会导致 spurious wakeup，是安全的
        global_task_count_.fetch_add(1, std::memory_order_release);

        global_queue_.enqueue(*t_prod_token, std::move(task));

        notify_one_worker();
    }

    void Scheduler::notify_one_worker()
    {
        if (sleeping_workers_.load(std::memory_order_relaxed) > 0)
        {
            std::lock_guard<std::mutex> lock(sleep_mtx_);
            sleep_cv_.notify_one();
        }
    }

    void Scheduler::worker_entry(int id)
    {
        t_worker_id = id;
        t_prod_token = std::make_unique<moodycamel::ProducerToken>(global_queue_);
        t_cons_token = std::make_unique<moodycamel::ConsumerToken>(global_queue_);

#ifdef __linux__
        std::string name = "Aegis-W-" + std::to_string(id);
        pthread_setname_np(pthread_self(), name.c_str());
#endif

        aegis::Log::instance().debug("Worker [{}] initialized.", id);

        const int MAX_SPINS = 4000;
        int spin_count = 0;

        while (running_)
        {
            SchedulerTask task = nullptr;

            if (auto t = try_local_pop(id))
                task = std::move(*t);
            else if (auto t = try_global_pop(id))
                task = std::move(*t);
            else if (auto t = try_steal(id))
                task = std::move(*t);

            if (task)
            {
                spin_count = 0;
                execute_actor(std::move(task));
                continue;
            }

            // --- Idle Strategy ---

            if (spin_count < MAX_SPINS)
            {
                spin_count++;
                _mm_pause();
                continue;
            }

            sleeping_workers_.fetch_add(1);
            {
                std::unique_lock<std::mutex> lock(sleep_mtx_);

                if (!running_)
                    break;

                // [Fix B] 使用 global_task_count_ 替代 size_approx()
                // 这保证了线性一致性：只要 count > 0，我们就不会因为 size_approx 的误差而沉睡
                sleep_cv_.wait_for(lock, std::chrono::milliseconds(1), [this]
                                   { return !running_ || global_task_count_.load(std::memory_order_acquire) > 0; });
            }
            sleeping_workers_.fetch_sub(1);

            spin_count = 0;
        }
    }

    void Scheduler::execute_actor(SchedulerTask task)
    {
        // 建议将来在这里加 try-catch 保护
        Actor::set_current(task.get());
        bool has_more = task->process_batch(100);
        Actor::set_current(nullptr);

        if (has_more)
        {
            dispatch(std::move(task));
        }
    }

    std::optional<SchedulerTask> Scheduler::try_local_pop(int id)
    {
        auto &lq = local_queues_[id];
        if (lq->q.empty())
            return std::nullopt;

        std::lock_guard<aegis::common::SpinLock> lock(lq->lock);
        // 使用 lock_guard 自动管理 SpinLock 更好

        SchedulerTask t = lq->q.pop();
        if (t)
            return t;
        return std::nullopt;
    }

    std::optional<SchedulerTask> Scheduler::try_global_pop(int id)
    {
        SchedulerTask t;
        constexpr int BATCH_SIZE = 16;
        SchedulerTask buffer[BATCH_SIZE];

        // 此时 count 可能包含正被别人 pop 的任务，但不影响我们尝试 dequeue
        size_t count = global_queue_.try_dequeue_bulk(*t_cons_token, buffer, BATCH_SIZE);

        if (count == 0)
            return std::nullopt;

        // [Fix B] 成功取出任务后，原子递减计数器
        // 保证 count 最终一致性
        global_task_count_.fetch_sub(count, std::memory_order_release);

        t = std::move(buffer[0]);

        if (count > 1)
        {
            auto &lq = local_queues_[id];
            std::lock_guard<aegis::common::SpinLock> lock(lq->lock);
            for (size_t i = 1; i < count; ++i)
            {
                lq->q.push(std::move(buffer[i]));
            }
        }

        return t;
    }

    std::optional<SchedulerTask> Scheduler::try_steal(int thief_id)
    {
        int num_workers = workers_.size();
        if (num_workers <= 1)
            return std::nullopt;

        // [Fix A] 使用 Xorshift32 生成随机索引，极快且无锁
        uint32_t r = fast_rand();
        int start_idx = r % num_workers;

        for (int i = 0; i < 3; ++i)
        {
            int victim_id = (start_idx + i) % num_workers;

            if (victim_id == thief_id)
                continue;

            auto &victim_q = local_queues_[victim_id];

            if (victim_q->lock.try_lock())
            {
                SchedulerTask t = victim_q->q.steal();
                victim_q->lock.unlock();

                if (t)
                    return t;
            }
        }

        return std::nullopt;
    }

} // namespace aegis::core