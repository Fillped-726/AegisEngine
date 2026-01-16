#pragma once

#include <cstddef> // size_t
#include <utility> // std::forward
#include <memory>
#include <vector>
#include <type_traits>
#include <concepts> // C++20
#include <new>

#include "concurrentqueue.h"
#include "aegisLog.h" // 集成日志系统

namespace aegis::core
{
    // C++20 Concept: 检测是否有 reset() 方法
    template <typename T>
    concept Resettable = requires(T t) {
        t.reset();
    };

    /**
     * @brief 线程本地缓存对象池 (Thread-Caching Object Pool)
     * @details 结合了 Thread Local Storage (L1 Cache) 和全局无锁队列 (L2 Cache)
     * @tparam T 对象类型
     * @tparam GlobalMaxSize 全局池最大容量
     * @tparam LocalBatchSize 本地缓存/批量搬运的大小
     */
    template <typename T, size_t GlobalMaxSize = 100000, size_t LocalBatchSize = 128>
    class ObjectPool
    {
    public:
        // 前置声明
        struct Deleter;

        // 智能指针定义，使用自定义 Deleter 自动回收
        using Ptr = std::unique_ptr<T, Deleter>;

        // 自定义删除器
        struct Deleter
        {
            void operator()(T *ptr) const
            {
                if (ptr)
                {
                    ObjectPool::instance().release(ptr);
                }
            }
        };

        // Meyers Singleton
        static ObjectPool &instance()
        {
            static ObjectPool inst;
            return inst;
        }

        // --- 核心接口 ---

        /**
         * @brief 获取对象
         * @param args 构造参数或 reset 参数
         * @return Ptr 智能指针
         */
        template <typename... Args>
        Ptr acquire(Args &&...args)
        {
            T *ptr = nullptr;

            // 1. 尝试从线程本地缓存取 (L1 Cache - 无锁，极速)
            if (!local_cache_.ptrs.empty())
            {
                ptr = local_cache_.ptrs.back();
                local_cache_.ptrs.pop_back();
            }
            // 2. 本地缓存为空，去全局池批量获取 (L2 Cache - 原子操作)
            else
            {
                // 尝试从全局队列搬运一批对象到本地
                size_t count = global_queue_.try_dequeue_bulk(local_cache_.cons_token, bulk_buffer_, LocalBatchSize);

                if (count > 0)
                {
                    // 取出一个直接给用户
                    ptr = bulk_buffer_[--count];

                    // 剩余的填充到本地缓存
                    if (count > 0)
                    {
                        local_cache_.ptrs.insert(local_cache_.ptrs.end(), bulk_buffer_, bulk_buffer_ + count);
                    }

                    // 可选：记录批量搬运日志 (Verbose/Debug 级别)
                    // aegis::Log::instance().debug("Refilled local cache from global pool. Count: {}", count);
                }
            }

            // 3. 全局池也没了，必须分配新内存 (Cold Path)
            if (!ptr) [[unlikely]]
            {
                // 日志记录：这是性能损耗点，值得关注
                // aegis::Log::instance().debug("Pool miss. Allocating new object of type: {}", typeid(T).name());
                ptr = new T(std::forward<Args>(args)...);
            }
            else
            {
                // 复用逻辑
                if constexpr (Resettable<T>)
                {
                    ptr->reset(std::forward<Args>(args)...);
                }
                else
                {
                    // 如果对象不可简单析构，需显式析构旧数据
                    if constexpr (!std::is_trivially_destructible_v<T>)
                    {
                        ptr->~T();
                    }

                    // 异常安全保护：Placement New
                    try
                    {
                        new (ptr) T(std::forward<Args>(args)...);
                    }
                    catch (...)
                    {
                        // 构造失败，必须释放这块裸内存，否则泄漏
                        ::operator delete(ptr);
                        aegis::Log::instance().error("Placement new failed in ObjectPool. Memory released.");
                        throw;
                    }
                }
            }

            return Ptr(ptr, Deleter{}); // Deleter 不需要 this 指针，它是无状态的或者访问单例
        }

        /**
         * @brief 归还对象
         */
        void release(T *ptr)
        {
            if (!ptr)
                return;

            // 1. 尝试放回本地缓存
            if (local_cache_.ptrs.size() < LocalBatchSize)
            {
                local_cache_.ptrs.push_back(ptr);
            }
            // 2. 本地满了，触发批量回写 (Flush to Global)
            else
            {
                local_cache_.ptrs.push_back(ptr);

                // 移动一半容量到全局，避免频繁在临界值抖动
                const size_t move_count = LocalBatchSize / 2;

                // 检查：确保计算的迭代器范围有效
                if (local_cache_.ptrs.size() < move_count) [[unlikely]]
                {
                    // 理论上不可达，但作为防御性编程
                    return;
                }

                auto start_it = local_cache_.ptrs.end() - move_count;
                auto end_it = local_cache_.ptrs.end();

                if (global_queue_.size_approx() < GlobalMaxSize)
                {
                    global_queue_.enqueue_bulk(local_cache_.prod_token, start_it, move_count);
                }
                else
                {
                    // 全局池满，销毁多余对象
                    for (auto it = start_it; it != end_it; ++it)
                    {
                        delete *it;
                    }
                    // aegis::Log::instance().warn("Global pool full. Dropped {} objects.", move_count);
                }

                // 移除本地缓存中已处理的指针
                local_cache_.ptrs.resize(local_cache_.ptrs.size() - move_count);
            }
        }

    private:
        ObjectPool() = default;

        ~ObjectPool()
        {
            // 1. 清理全局队列
            T *ptr;
            while (global_queue_.try_dequeue(ptr))
            {
                delete ptr;
            }
            aegis::Log::instance().info("ObjectPool destroyed. Global queue cleared.");
        }

        // --- 内部类：线程缓存守卫 ---
        struct alignas(std::hardware_destructive_interference_size) ThreadLocalCache
        {
            std::vector<T *> ptrs;
            moodycamel::ProducerToken prod_token;
            moodycamel::ConsumerToken cons_token;

            ThreadLocalCache()
                : prod_token(ObjectPool::instance().global_queue_),
                  cons_token(ObjectPool::instance().global_queue_)
            {
                ptrs.reserve(LocalBatchSize);
            }

            ~ThreadLocalCache()
            {
                // 线程退出时，直接销毁本地缓存的对象
                // 此时不应访问全局单例，防止由析构顺序导致的崩溃
                size_t leaked_count = ptrs.size();
                for (T *ptr : ptrs)
                {
                    delete ptr;
                }
                ptrs.clear();
            }
        };

        moodycamel::ConcurrentQueue<T *> global_queue_;

        // 线程本地存储 (L1 Cache)
        inline static thread_local ThreadLocalCache local_cache_;

        // 临时搬运 buffer，thread_local 避免栈溢出或重复分配
        inline static thread_local T *bulk_buffer_[LocalBatchSize];
    };

} // namespace aegis::core