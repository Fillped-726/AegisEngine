#include "aegis/core/scheduler.h"
#include "aegis/common/aegisLog.h"
#include <chrono>
#include <immintrin.h> // _mm_pause

#ifdef __linux__
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace aegis::core
{
    // --- TLS 上下文保持不变 ---
    static thread_local int t_worker_id = -1;
    static thread_local std::unique_ptr<moodycamel::ProducerToken> t_prod_token;
    static thread_local std::unique_ptr<moodycamel::ConsumerToken> t_cons_token;
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

    Scheduler::~Scheduler() { stop(); }

    void Scheduler::start(int num_workers)
    {
        if (running_)
            return;

        running_ = true;

        // [Change] 初始化 Lock-Free Queues
        local_queues_.resize(num_workers);
        for (int i = 0; i < num_workers; ++i)
        {
            local_queues_[i] = std::make_unique<WorkQueue>();
        }

        aegis::Log::instance().info("Scheduler (Lock-Free Chase-Lev) starting with {} workers...", num_workers);

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

        running_ = false;
        sleep_cv_.notify_all();

        for (auto &t : workers_)
        {
            if (t.joinable())
                t.join();
        }
        workers_.clear();
        local_queues_.clear();
    }

    void Scheduler::dispatch(SchedulerTask task)
    {
        if (!task)
            return;

        // [Path A] 本地队列 (Owner 极速模式)
        // 以前这里需要 lock.lock()，现在直接 push
        if (t_worker_id >= 0)
        {
            // 注意：push 可能会失败（队列满），如果满了需要 fallback 到全局队列
            // WorkStealingQueue::push 返回 bool
            if (local_queues_[t_worker_id]->push(task)) // 这里发生了拷贝，因为参数不是右值引用，优化点在下面
            {
                return;
            }
            // 如果本地满了，Fallthrough 到 Path B，放入全局队列
        }

        // [Path B] 全局队列
        if (!t_prod_token)
            t_prod_token = std::make_unique<moodycamel::ProducerToken>(global_queue_);

        global_queue_.enqueue(*t_prod_token, std::move(task));

        // 依然需要唤醒
        notify_one_worker();
    }

    void Scheduler::notify_one_worker()
    {
        // 只有当有工人睡着时才去拿锁 notify
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

        // 初始化随机种子
        t_rng_seed = init_seed();

#ifdef __linux__
        std::string name = "Aegis-W-" + std::to_string(id);
        pthread_setname_np(pthread_self(), name.c_str());
#endif

        const int MAX_SPINS = 4000;
        int spin_count = 0;

        while (running_)
        {
            SchedulerTask task = nullptr;

            // 1. 本地 (Lock-Free LIFO)
            if (auto t = try_local_pop(id))
                task = std::move(*t);
            // 2. 全局 (Lock-Free FIFO)
            else if (auto t = try_global_pop(id))
                task = std::move(*t);
            // 3. 窃取 (Lock-Free Steal)
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

            // 休眠逻辑
            sleeping_workers_.fetch_add(1);
            {
                std::unique_lock<std::mutex> lock(sleep_mtx_);

                if (!running_)
                    break;

                // [Change] 使用 size_approx() 替代 global_task_count_
                // 注意：这里只检测全局队列是否有任务。
                // 严格来说，Work Stealing 场景下，即使全局空，别的人本地可能有任务。
                // 但为了避免惊群和频繁唤醒，只在全局有任务时唤醒是合理的策略。
                // (更激进的策略是：如果 try_steal 失败多次再睡)
                sleep_cv_.wait_for(lock, std::chrono::milliseconds(1), [this]
                                   { return !running_ || global_queue_.size_approx() > 0; });
            }
            sleeping_workers_.fetch_sub(1);
            spin_count = 0;
        }
    }

    void Scheduler::execute_actor(Actor *actor) // 裸指针
    {
        Actor::set_current(actor);

        // 获取状态
        ActorState state = actor->process_batch(100);

        Actor::set_current(nullptr);

        switch (state)
        {
        case ActorState::Active:
            dispatch(actor); // 重新入队
            break;

        case ActorState::Idle:
            // 没事做，挂起 (不 delete，也不 dispatch)
            break;

        case ActorState::Dead:
            // [毒丸生效]
            // 只有在这里，在没有任何并发引用的情况下，安全自杀
            delete actor;
            break;
        }
    }

    std::optional<SchedulerTask> Scheduler::try_local_pop(int id)
    {
        // [Change] 无锁 Pop，直接调用
        // 这里的开销从 "Lock + Unlock" 降级为 "Atomic Load + Store"
        return local_queues_[id]->pop();
    }

    std::optional<SchedulerTask> Scheduler::try_global_pop(int id)
    {
        SchedulerTask t;
        // 简化逻辑：一次取一个，或者依然保留 Bulk
        // ConcurrentQueue 的 try_dequeue 是无锁的
        if (global_queue_.try_dequeue(*t_cons_token, t))
        {
            return t;
        }
        return std::nullopt;
    }

    std::optional<SchedulerTask> Scheduler::try_steal(int thief_id)
    {
        int num_workers = workers_.size();
        if (num_workers <= 1)
            return std::nullopt;

        // 随机选择受害者
        uint32_t r = fast_rand();
        int start_idx = r % num_workers;

        // 尝试偷 3 次，避免过度遍历造成总线风暴
        for (int i = 0; i < 3; ++i)
        {
            int victim_id = (start_idx + i) % num_workers;
            if (victim_id == thief_id)
                continue;

            // [Change] 无锁 Steal
            // 这里发生了本质变化：不再需要 try_lock()
            // 如果发生竞争，CAS 会自动失败，我们直接找下一个受害者
            if (auto t = local_queues_[victim_id]->steal())
            {
                return t;
            }
        }
        return std::nullopt;
    }

} // namespace aegis::core