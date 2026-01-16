#pragma once

#include "aegis/common/aegisLog.h"
#include "aegis/common/spinLock.h"
#include "aegis/common/objectPool.h"
#include "aegis/common/intrusive_list.h" // [Require] 必须引入侵入式链表

#include <vector>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <mutex>         // for std::lock_guard
#include <sys/timerfd.h> // for timerfd
#include <unistd.h>      // for close

namespace aegis::core
{
    // 核心参数常量
    constexpr int TVR_BITS = 8;
    constexpr int TVN_BITS = 6;
    constexpr int TVR_SIZE = 1 << TVR_BITS; // 256
    constexpr int TVN_SIZE = 1 << TVN_BITS; // 64

    constexpr int TVR_MASK = TVR_SIZE - 1;
    constexpr int TVN_MASK = TVN_SIZE - 1;

    using TimerCallback = std::function<void()>;

    struct TimerNode : public aegis::common::IntrusiveListNode
    {
        uint64_t expires; // 绝对到期时间
        TimerCallback cb;
        uint64_t id = 0;

        // ObjectPool 必需接口
        void reset()
        {
            expires = 0;
            cb = nullptr;
            id = 0;
            prev = nullptr;
            next = nullptr;
        }
    };

    // 定义 Pool 类型
    using TimerNodePool = aegis::core::ObjectPool<TimerNode, 100000, 128>;

    class HierarchicalTimeWheel
    {
    public:
        // [New] 定义 TimerId 类型
        using TimerId = uint64_t;
        // [Change] 改为单例 (Singleton) 以匹配全局唯一的 timerfd 管理需求
        static HierarchicalTimeWheel &instance()
        {
            static HierarchicalTimeWheel inst;
            return inst;
        }

        /**
         * @brief 初始化 timerfd
         */
        void init()
        {
            if (timer_fd_ >= 0)
                return;
            timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (timer_fd_ < 0)
                return; // Error handling needed

            struct itimerspec new_value;
            new_value.it_value.tv_sec = 0;
            new_value.it_value.tv_nsec = 10 * 1000 * 1000; // 10ms
            new_value.it_interval.tv_sec = 0;
            new_value.it_interval.tv_nsec = 10 * 1000 * 1000; // 10ms
            timerfd_settime(timer_fd_, 0, &new_value, nullptr);

            aegis::Log::instance().info("TimerWheel Initialized. FD: {}", timer_fd_);
        }

        int get_fd() const { return timer_fd_; }

        /**
         * @brief 添加定时器 (线程安全)
         */
        TimerId add_timer(uint32_t delay, TimerCallback cb)
        {
            // [Fix] 获取裸指针，手动管理生命周期
            auto node = TimerNodePool::instance().acquire().release();

            // 此时存相对时间 (delay)，tick 时转为绝对时间
            node->expires = delay;
            node->cb = std::move(cb);
            // [New] 自动生成唯一 ID (从 1 开始)
            node->id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);

            {
                std::lock_guard<aegis::common::SpinLock> lock(lock_);
                pending_adds_.push_back(node);
            }
            return node->id;
        }

        void cancel_timer(TimerId id)
        {
            if (id == 0)
                return;
            std::lock_guard<aegis::common::SpinLock> lock(lock_);
            pending_cancels_.push_back(id);
        }

        /**
         * @brief 驱动函数 (由 IO 线程调用)
         */
        void tick()
        {
            std::vector<TimerNode *> local_adds;
            std::vector<TimerId> local_cancels;

            // 1. 双缓冲交换
            {
                std::lock_guard<aegis::common::SpinLock> lock(lock_);
                if (!pending_adds_.empty())
                    local_adds.swap(pending_adds_);
                if (!pending_cancels_.empty())
                    local_cancels.swap(pending_cancels_);
            }

            // 2. 处理添加
            for (TimerNode *node : local_adds)
            {
                // 转为绝对时间
                node->expires = current_tick_ + node->expires;
                if (node->id != 0)
                    timer_map_[node->id] = node;
                add_timer_internal(node);
            }

            // 3. 处理取消 (惰性删除)
            for (int id : local_cancels)
            {
                auto it = timer_map_.find(id);
                if (it != timer_map_.end())
                {
                    it->second->cb = nullptr; // 标记失效
                    timer_map_.erase(it);
                }
            }

            // --- 核心时间轮逻辑 ---

            int index = current_tick_ & TVR_MASK;

            if (index == 0 && current_tick_ > 0)
            {
                cascade_timers();
            }

            // [Fix] 使用侵入式链表
            auto &list = tv1_[index];
            while (TimerNode *node = list.pop_front())
            {
                if (node->cb)
                {
                    // aegis::Log::instance().debug("Exec Timer: ID={}, Tick={}", node->id, current_tick_);
                    node->cb();
                    if (node->id > 0)
                        timer_map_.erase(node->id);
                }

                // [Fix] 显式回收内存
                TimerNodePool::instance().release(node);
            }

            current_tick_++;
        }

    private:
        HierarchicalTimeWheel() = default;
        ~HierarchicalTimeWheel()
        {
            if (timer_fd_ >= 0)
                close(timer_fd_);
        }

        void add_timer_internal(TimerNode *node)
        {
            uint64_t expires = node->expires;
            // 容错：如果已经过期，强制设为当前时间+1，或者立即执行
            // 这里为了简单，假设总是未来时间。如果是过去时间，会落入错误的 slot，需要额外判断
            if (expires <= current_tick_)
                expires = current_tick_;

            uint64_t idx = expires - current_tick_;
            int i = 0;

            if (idx < TVR_SIZE)
            {
                i = expires & TVR_MASK;
                tv1_[i].push_back(node);
            }
            else if (idx < (1ULL << (TVR_BITS + TVN_BITS)))
            {
                i = (expires >> TVR_BITS) & TVN_MASK;
                tv2_[i].push_back(node);
            }
            else if (idx < (1ULL << (TVR_BITS + 2 * TVN_BITS)))
            {
                i = (expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK;
                tv3_[i].push_back(node);
            }
            else if (idx < (1ULL << (TVR_BITS + 3 * TVN_BITS)))
            {
                i = (expires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK;
                tv4_[i].push_back(node);
            }
            else
            {
                i = (expires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK;
                tv5_[i].push_back(node);
            }
        }

        // [Fix] 零拷贝级联
        int cascade(aegis::common::IntrusiveList<TimerNode> &list)
        {
            int count = 0;
            while (TimerNode *node = list.pop_front())
            {
                add_timer_internal(node);
                count++;
            }
            return count;
        }

        void cascade_timers()
        {
            uint64_t ct = current_tick_;
            int i2 = (ct >> TVR_BITS) & TVN_MASK;
            cascade(tv2_[i2]);
            if (i2 == 0)
            {
                int i3 = (ct >> (TVR_BITS + TVN_BITS)) & TVN_MASK;
                cascade(tv3_[i3]);
                if (i3 == 0)
                {
                    int i4 = (ct >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK;
                    cascade(tv4_[i4]);
                    if (i4 == 0)
                    {
                        int i5 = (ct >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK;
                        cascade(tv5_[i5]);
                    }
                }
            }
        }

        uint64_t current_tick_ = 0;
        int timer_fd_ = -1;
        // [New] 原子 ID 生成器 (从 1 开始，0 保留)
        std::atomic<TimerId> next_timer_id_{1};

        aegis::common::SpinLock lock_;
        std::vector<TimerNode *> pending_adds_;
        std::vector<TimerId> pending_cancels_;
        std::unordered_map<TimerId, TimerNode *> timer_map_;

        // [Fix] 使用侵入式链表，而不是 std::list
        aegis::common::IntrusiveList<TimerNode> tv1_[TVR_SIZE];
        aegis::common::IntrusiveList<TimerNode> tv2_[TVN_SIZE];
        aegis::common::IntrusiveList<TimerNode> tv3_[TVN_SIZE];
        aegis::common::IntrusiveList<TimerNode> tv4_[TVN_SIZE];
        aegis::common::IntrusiveList<TimerNode> tv5_[TVN_SIZE];
    };

} // namespace aegis::core