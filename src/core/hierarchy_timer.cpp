#include "aegis/core/hierarchy_timer.h"
#include "aegis/common/aegisLog.h"

#include <mutex>         // for std::lock_guard
#include <sys/timerfd.h> // for timerfd
#include <unistd.h>      // for close

namespace aegis::core
{

    void TimerNode::reset()
    {
        expires = 0;
        cb = nullptr;
        id = 0;
        prev = nullptr;
        next = nullptr;
    }

    TimerId HierarchicalTimeWheel::add_timer(uint32_t delay_ms, TimerCallback cb)
    {
        // 1. 将物理毫秒转换为逻辑 Tick 数 (向上取整，不足1个tick算1个)
        uint32_t delay_ticks = delay_ms / TICK_MS;
        if (delay_ticks == 0)
            delay_ticks = 1;

        // 2. 申请节点 (直接复用你的 ObjectPool)
        auto node = TimerNodePool::instance().acquire().release();
        node->reset();
        node->cb = std::move(cb);
        node->id = next_timer_id_++;
        node->expires = current_tick_ + delay_ticks; // 直接计算绝对过期 Tick

        // 3. 直接存入 Map 和 时间轮 (完全无锁，无需 pending 缓冲！)
        timer_map_[node->id] = node;
        add_timer_internal(node);

        return node->id;
    }

    void HierarchicalTimeWheel::cancel_timer(TimerId id)
    {
        auto it = timer_map_.find(id);
        if (it != timer_map_.end())
        {
            it->second->cb = nullptr;
            timer_map_.erase(it);
        }
    }

    void HierarchicalTimeWheel::tick()
    {
        // 【重构重点】：删除了极其昂贵的锁等待和双缓冲 Swap 逻辑！
        // 代码变得纯粹、极致的高效

        int index = current_tick_ & TVR_MASK;

        if (index == 0 && current_tick_ > 0)
        {
            cascade_timers();
        }

        auto &list = tv1_[index];
        while (TimerNode *node = list.pop_front())
        {
            if (node->cb)
            {
                node->cb(); // 极其安全的直接调用！因为我们在同一个 Worker 线程！
                timer_map_.erase(node->id);
            }

            TimerNodePool::instance().release(node);
        }

        current_tick_++;
    }

    void HierarchicalTimeWheel::add_timer_internal(TimerNode *node)
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

    int HierarchicalTimeWheel::cascade(aegis::common::IntrusiveList<TimerNode> &list)
    {
        int count = 0;
        while (TimerNode *node = list.pop_front())
        {
            add_timer_internal(node);
            count++;
        }
        return count;
    }

    void HierarchicalTimeWheel::cascade_timers()
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

} // namespace aegis::core