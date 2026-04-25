#pragma once

#include "aegis/common/objectPool.h"
#include "aegis/common/intrusive_list.h" // [Require] 必须引入侵入式链表

#include <functional>
#include <cstdint>
#include <unordered_map>

namespace aegis::core
{
    // 核心参数常量
    constexpr int TVR_BITS = 8;
    constexpr int TVN_BITS = 6;
    constexpr int TVR_SIZE = 1 << TVR_BITS; // 256
    constexpr int TVN_SIZE = 1 << TVN_BITS; // 64

    constexpr int TVR_MASK = TVR_SIZE - 1;
    constexpr int TVN_MASK = TVN_SIZE - 1;

    constexpr uint32_t TICK_MS = 50;

    using TimerCallback = std::function<void()>;
    using TimerId = uint64_t;

    struct TimerNode : public aegis::common::IntrusiveListNode
    {
        uint64_t expires;
        TimerCallback cb;
        TimerId id = 0;

        void reset();
    };

    // 定义 Pool 类型
    using TimerNodePool = aegis::core::ObjectPool<TimerNode, 100000, 128>;

    class HierarchicalTimeWheel
    {
    public:
        HierarchicalTimeWheel() = default;
        ~HierarchicalTimeWheel() = default;

        /**
         * @brief 添加定时器 (线程安全)
         */
        TimerId add_timer(uint32_t delay, TimerCallback cb);

        void cancel_timer(TimerId id);

        /**
         * @brief 驱动函数 (由 IO 线程调用)
         */
        void tick();

    private:
        void add_timer_internal(TimerNode *node);

        // [Fix] 零拷贝级联
        int cascade(aegis::common::IntrusiveList<TimerNode> &list);

        void cascade_timers();

        uint64_t current_tick_ = 0;
        uint64_t next_timer_id_ = 1;

        std::unordered_map<TimerId, TimerNode *> timer_map_;

        // [Fix] 使用侵入式链表，而不是 std::list
        aegis::common::IntrusiveList<TimerNode> tv1_[TVR_SIZE];
        aegis::common::IntrusiveList<TimerNode> tv2_[TVN_SIZE];
        aegis::common::IntrusiveList<TimerNode> tv3_[TVN_SIZE];
        aegis::common::IntrusiveList<TimerNode> tv4_[TVN_SIZE];
        aegis::common::IntrusiveList<TimerNode> tv5_[TVN_SIZE];
    };

} // namespace aegis::core