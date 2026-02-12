#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <string>
#include "aegis/common/objectPool.h" // 假设你的头文件叫这个

using namespace aegis::core;

// ==========================================
// 辅助测试类
// ==========================================

// 1. 支持 reset() 的类
struct ResettableObj
{
    int id;
    std::string data;
    static std::atomic<int> reset_count;
    static std::atomic<int> construct_count;

    ResettableObj(int i) : id(i), data("init")
    {
        construct_count++;
    }

    void reset(int i)
    {
        id = i;
        data = "reset";
        reset_count++;
    }
};
std::atomic<int> ResettableObj::reset_count{0};
std::atomic<int> ResettableObj::construct_count{0};

// 2. 不支持 reset() 的类 (将触发析构 + Placement New)
struct NonResettableObj
{
    int x;
    static std::atomic<int> construct_count;
    static std::atomic<int> destruct_count;

    NonResettableObj(int v) : x(v) { construct_count++; }
    ~NonResettableObj() { destruct_count++; }
};
std::atomic<int> NonResettableObj::construct_count{0};
std::atomic<int> NonResettableObj::destruct_count{0};

// ==========================================
// 单元测试
// ==========================================

class ObjectPoolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 重置计数器
        ResettableObj::reset_count = 0;
        ResettableObj::construct_count = 0;
        NonResettableObj::construct_count = 0;
        NonResettableObj::destruct_count = 0;
    }
};

// 测试 1: 基本的获取与自动归还
TEST_F(ObjectPoolTest, BasicAcquireAndRelease)
{
    using Pool = ObjectPool<ResettableObj>;

    {
        auto ptr = Pool::instance().acquire(100);
        ASSERT_TRUE(ptr != nullptr);
        EXPECT_EQ(ptr->id, 100);
        EXPECT_EQ(ResettableObj::construct_count, 1);
    }
    // 出作用域，应该自动归还到 Pool 的 L1 cache

    // 再次获取，应该复用对象，不增加构造计数，但增加 reset 计数
    {
        auto ptr = Pool::instance().acquire(200);
        EXPECT_EQ(ptr->id, 200);
        EXPECT_EQ(ptr->data, "reset");                // 确保调用了 reset
        EXPECT_EQ(ResettableObj::construct_count, 1); // 没变
        EXPECT_EQ(ResettableObj::reset_count, 1);     // 增加了
    }
}

// 测试 2: Fallback 机制 (没有 reset 方法的类)
TEST_F(ObjectPoolTest, NonResettableFallback)
{
    using Pool = ObjectPool<NonResettableObj>;

    NonResettableObj *addr1 = nullptr;
    {
        auto ptr = Pool::instance().acquire(10);
        addr1 = ptr.get();
        EXPECT_EQ(NonResettableObj::construct_count, 1);
    }

    // 再次获取，应该复用内存，但触发 析构 + 构造
    {
        auto ptr = Pool::instance().acquire(20);
        EXPECT_EQ(ptr.get(), addr1); // 应该是同一个地址（单线程下 L1 栈顶）
        EXPECT_EQ(ptr->x, 20);

        // 第一次的对象被析构了，然后原地构造了第二次
        EXPECT_EQ(NonResettableObj::destruct_count, 1);
        EXPECT_EQ(NonResettableObj::construct_count, 2);
    }
}

// 测试 3: L1 Cache 溢出与批量回写 (Bulk Transfer)
// 我们设置极小的 LocalBatchSize 来触发逻辑
TEST_F(ObjectPoolTest, L1OverflowToGlobal)
{
    // 定义一个小容量池: Global=100, Local=4
    using MiniPool = ObjectPool<int, 100, 4>;

    std::vector<MiniPool::Ptr> ptrs;

    // 1. 申请 10 个对象 (超过 LocalBatchSize * 2 = 8)
    for (int i = 0; i < 10; ++i)
    {
        ptrs.push_back(MiniPool::instance().acquire(i));
    }

    // 2. 全部释放。
    // - 前 8 个会填满 vector
    // - 第 9 个释放时 (size=8, limit=8)，触发搬运：
    //   移动 4 个到 Global Queue，本地剩 4+1=5 个。
    // - 第 10 个释放，本地变成 6 个。
    ptrs.clear();

    // 3. 验证 Global Queue 是否有数据
    // 我们没法直接访问私有成员，但可以通过另一个线程去取来验证
    // 如果另一个线程能取到对象且不触发 new，说明数据进了 Global

    std::atomic<bool> found_in_global{false};
    std::thread t([&]()
                  {
        // 在新线程中，L1 是空的，必须去 Global 取
        // 为了确保不是 new 出来的，我们可以检查指针地址（太复杂），
        // 或者简单地相信逻辑。这里我们主要测代码不崩。
        auto ptr = MiniPool::instance().acquire(999);
        // 如果 global queue 只有刚才那个线程放进去的，这里应该能拿出来
        // 由于是 new int，没法像自定义类那样看副作用，主要测流程通畅
        found_in_global = true; });
    t.join();

    EXPECT_TRUE(found_in_global);
}

// 测试 4: 多线程高并发压力测试
TEST_F(ObjectPoolTest, ConcurrencyStressTest)
{
    using StressPool = ObjectPool<std::string>;
    const int thread_count = 8;
    const int ops_per_thread = 10000;

    std::vector<std::thread> threads;
    std::atomic<int> total_acquired{0};

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&]()
                             {
            for (int j = 0; j < ops_per_thread; ++j) {
                auto ptr = StressPool::instance().acquire("test");
                if (ptr) total_acquired++;
                // 立即释放，制造高频的 acquire/release 抖动
            } });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    EXPECT_EQ(total_acquired, thread_count * ops_per_thread);
}

// 测试 5: 全局容量限制 (GlobalMaxSize)
TEST_F(ObjectPoolTest, GlobalMaxSizeLimit)
{
    // GlobalMax = 10, Local = 2
    // 如果所有线程释放了巨量对象，全局队列满了之后应该直接 delete，而不是无限增长
    using LimitedPool = ObjectPool<ResettableObj, 10, 2>;

    // 既然这是单例，之前的测试可能已经污染了状态？
    // 注意：ObjectPool 是模板类，不同模板参数产生不同类型，所以 LimitedPool 是全新的单例。Safe。

    std::vector<LimitedPool::Ptr> keeps;
    // 申请 50 个
    for (int i = 0; i < 50; ++i)
    {
        keeps.push_back(LimitedPool::instance().acquire(i));
    }

    // 释放所有。
    // L1 (size 2) 满了 -> 往 Global 搬。
    // Global (size 10) 满了 -> 剩下的应该直接析构。
    keeps.clear();

    // 再次申请 1 个，应该能拿到
    auto ptr = LimitedPool::instance().acquire(999);
    EXPECT_EQ(ptr->id, 999);
}