#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <numeric>
#include <atomic>
#include <latch> // C++20
#include <random>
#include <optional> // 必须包含

#include "aegis/common/work_stealing_queue.h"

using namespace aegis::common;

// 【修改点1】T 现在需要显式定义为指针类型，因为队列本身变成了泛型 T
using TestTask = int *;

class WorkStealingQueueTest : public ::testing::Test
{
protected:
    // WorkStealingQueue<int*, 128>
    WorkStealingQueue<TestTask, 128> queue;
};

// 1. 基础功能测试：Owner 的 LIFO 行为
TEST_F(WorkStealingQueueTest, OwnerPushPop_LIFO)
{
    int *t1 = new int(1);
    int *t2 = new int(2);
    int *t3 = new int(3);

    // Push 接口接收 T (即 int*)，这里无需改动
    EXPECT_TRUE(queue.push(t1));
    EXPECT_TRUE(queue.push(t2));
    EXPECT_TRUE(queue.push(t3));

    // 【修改点2】Pop 返回 std::optional<int*>
    auto p3_opt = queue.pop();
    ASSERT_TRUE(p3_opt.has_value()); // 检查是否有值
    int *p3 = *p3_opt;               // 解包拿到指针
    EXPECT_EQ(*p3, 3);
    delete p3;

    auto p2_opt = queue.pop();
    ASSERT_TRUE(p2_opt); // 简写：隐式 bool 转换
    int *p2 = *p2_opt;
    EXPECT_EQ(*p2, 2);
    delete p2;

    auto p1_opt = queue.pop();
    ASSERT_TRUE(p1_opt);
    int *p1 = *p1_opt;
    EXPECT_EQ(*p1, 1);
    delete p1;

    // 空了应该返回 std::nullopt
    auto empty_opt = queue.pop();
    EXPECT_FALSE(empty_opt.has_value());
}

// 2. 基础功能测试：Thief 的 FIFO 行为
TEST_F(WorkStealingQueueTest, ThiefSteal_FIFO)
{
    int *t1 = new int(100);
    int *t2 = new int(200);
    int *t3 = new int(300);

    queue.push(t1);
    queue.push(t2);
    queue.push(t3);

    std::thread thief([&]()
                      {
        // 【修改点3】Steal 返回 std::optional<int*>
        auto s1_opt = queue.steal();
        ASSERT_TRUE(s1_opt);
        int* s1 = *s1_opt;
        EXPECT_EQ(*s1, 100); 
        delete s1;

        auto s2_opt = queue.steal();
        ASSERT_TRUE(s2_opt);
        int* s2 = *s2_opt;
        EXPECT_EQ(*s2, 200);
        delete s2; });
    thief.join();

    // Owner 此时去 pop
    auto p3_opt = queue.pop();
    ASSERT_TRUE(p3_opt);
    int *p3 = *p3_opt;
    EXPECT_EQ(*p3, 300);
    delete p3;
}

// 3. 边界测试：容量限制
TEST_F(WorkStealingQueueTest, CapacityLimit)
{
    std::vector<int *> pointers;
    // 填满 128 个
    for (int i = 0; i < 128; ++i)
    {
        int *t = new int(i);
        pointers.push_back(t);
        EXPECT_TRUE(queue.push(t));
    }

    // 第 129 个应该失败
    int *overflow = new int(999);
    EXPECT_FALSE(queue.push(overflow));
    delete overflow;

    // 清理内存
    for (int i = 0; i < 128; ++i)
    {
        auto p_opt = queue.pop();
        ASSERT_TRUE(p_opt);
        delete *p_opt; // 直接解引用删除
    }
}

// 4. 压力测试：高并发竞争验证 (核心测试)
TEST_F(WorkStealingQueueTest, ConcurrencyStressTest)
{
    const int NUM_ITEMS = 400000;
    const int NUM_THIEVES = 4;

    std::atomic<long long> total_sum{0};
    std::atomic<int> consumed_count{0};

    std::latch start_latch(NUM_THIEVES + 1);

    // Thief 逻辑
    auto thief_func = [&]()
    {
        start_latch.arrive_and_wait();
        while (consumed_count.load(std::memory_order_relaxed) < NUM_ITEMS)
        {
            // 【修改点4】适配 optional
            auto task_opt = queue.steal();
            if (task_opt) // 有值
            {
                int *task = *task_opt; // 解包
                total_sum.fetch_add(*task, std::memory_order_relaxed);
                consumed_count.fetch_add(1, std::memory_order_relaxed);
                delete task;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> thieves;
    for (int i = 0; i < NUM_THIEVES; ++i)
    {
        thieves.emplace_back(thief_func);
    }

    // Owner 逻辑
    long long expected_sum = 0;

    start_latch.arrive_and_wait();

    for (int i = 0; i < NUM_ITEMS; ++i)
    {
        int *task = new int(i + 1);
        expected_sum += (i + 1);

        while (!queue.push(task))
        {
            // 如果满了，自己 Pop 一个处理
            auto own_task_opt = queue.pop();
            if (own_task_opt)
            {
                int *own_task = *own_task_opt;
                total_sum.fetch_add(*own_task, std::memory_order_relaxed);
                consumed_count.fetch_add(1, std::memory_order_relaxed);
                delete own_task;
            }
            else
            {
                std::this_thread::yield();
            }
        }

        if (i % 10 == 0)
        {
            auto t_opt = queue.pop();
            if (t_opt)
            {
                int *t = *t_opt;
                total_sum.fetch_add(*t, std::memory_order_relaxed);
                consumed_count.fetch_add(1, std::memory_order_relaxed);
                delete t;
            }
        }
    }

    // 收尾
    while (consumed_count.load() < NUM_ITEMS)
    {
        auto t_opt = queue.pop();
        if (t_opt)
        {
            int *t = *t_opt;
            total_sum.fetch_add(*t, std::memory_order_relaxed);
            consumed_count.fetch_add(1, std::memory_order_relaxed);
            delete t;
        }
        else
        {
            std::this_thread::yield();
        }
    }

    for (auto &t : thieves)
    {
        if (t.joinable())
            t.join();
    }

    EXPECT_EQ(consumed_count.load(), NUM_ITEMS) << "Lost or duplicated tasks!";
    EXPECT_EQ(total_sum.load(), expected_sum) << "Data corruption detected! Sum mismatch.";
}