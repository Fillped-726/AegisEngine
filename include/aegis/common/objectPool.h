#pragma once

#include <cstddef>
#include <utility>
#include <memory>
#include <vector>
#include <type_traits>
#include <concepts>
#include <new>
#include <atomic> // 恢复 atomic

#include "concurrentqueue.h"
#include "aegisLog.h"

namespace aegis::core
{
    template <typename T, typename... Args>
    concept Resettable = requires(T t, Args &&...args) {
        t.reset(std::forward<Args>(args)...);
    };

    template <typename T, size_t GlobalMaxSize = 100000, size_t LocalBatchSize = 128>
    class ObjectPool
    {
    public:
        struct Deleter
        {
            void operator()(T *ptr) const
            {
                if (ptr)
                    ObjectPool::instance().release(ptr);
            }
        };

        using Ptr = std::unique_ptr<T, Deleter>;

        static ObjectPool &instance()
        {
            static ObjectPool inst;
            return inst;
        }

        // 禁止拷贝和移动
        ObjectPool(const ObjectPool &) = delete;
        ObjectPool &operator=(const ObjectPool &) = delete;

        template <typename... Args>
        Ptr acquire(Args &&...args)
        {
            // 防御：程序退出阶段直接 new，避免访问已销毁的队列
            if (!is_active_.load(std::memory_order_acquire)) [[unlikely]]
            {
                return Ptr(new T(std::forward<Args>(args)...));
            }

            T *ptr = nullptr;
            auto &local = GetLocalCache();

            // 1. L1 Cache Hit
            if (!local.ptrs.empty())
            {
                ptr = local.ptrs.back();
                local.ptrs.pop_back();
            }
            // 2. L1 Miss -> L2 (Global)
            else
            {
                // 注意：这里使用 local.bulk_buffer，保证缓存局部性
                size_t count = global_queue_.try_dequeue_bulk(local.cons_token, local.bulk_buffer, LocalBatchSize);
                if (count > 0)
                {
                    ptr = local.bulk_buffer[--count];
                    if (count > 0)
                    {
                        local.ptrs.insert(local.ptrs.end(), local.bulk_buffer, local.bulk_buffer + count);
                    }
                }
            }

            // 3. Alloc New (Cold Path)
            if (!ptr) [[unlikely]]
            {
                ptr = new T(std::forward<Args>(args)...);
            }
            else
            {
                construct_or_reset(ptr, std::forward<Args>(args)...);
            }

            return Ptr(ptr);
        }

        void release(T *ptr)
        {
            if (!ptr)
                return;

            // 防御：程序退出中，直接析构，不回全局
            if (!is_active_.load(std::memory_order_acquire)) [[unlikely]]
            {
                delete ptr;
                return;
            }

            auto &local = GetLocalCache();

            // 1. Push to local
            local.ptrs.push_back(ptr);

            // 2. Watermark check (Version B's tuned logic)
            if (local.ptrs.size() >= LocalBatchSize)
            {
                // 仅搬运一半，留一半热数据
                const size_t move_count = LocalBatchSize / 2;

                // 优化：将 vector 尾部的数据搬运到 buffer
                // 虽然 vector 尾部是热数据，但为了 vector 操作的 O(1)，我们通常只能切尾部
                auto start_it = local.ptrs.end() - move_count;

                // 直接使用 copy 到 bulk_buffer，这比迭代器范围直接塞给 queue 可能稍微慢一点点拷贝，
                // 但 concurrentqueue 的 enqueue_bulk 原生支持迭代器，所以可以直接传迭代器。
                // 不过，为了兼容性，我们还是用 bulk_buffer 做中转（因为 try_dequeue_bulk 必须用 buffer）
                // 这里直接传迭代器给 global_queue 也是可以的，省一次拷贝到 buffer

                if (global_queue_.size_approx() < GlobalMaxSize)
                {
                    // Moodycamel queue supports iterator traits
                    global_queue_.enqueue_bulk(local.prod_token, start_it, move_count);
                }
                else
                {
                    for (auto it = start_it; it != local.ptrs.end(); ++it)
                        delete *it;
                }

                local.ptrs.resize(local.ptrs.size() - move_count);
            }
        }

    private:
        ObjectPool() { is_active_.store(true, std::memory_order_release); }

        ~ObjectPool()
        {
            is_active_.store(false, std::memory_order_release);
            T *ptr;
            while (global_queue_.try_dequeue(ptr))
                delete ptr;
        }

        // 统一封装 Reset 逻辑，包含异常安全处理
        template <typename... Args>
        void construct_or_reset(T *ptr, Args &&...args)
        {
            if constexpr (Resettable<T, Args...>)
            {
                ptr->reset(std::forward<Args>(args)...);
            }
            else
            {
                if constexpr (!std::is_trivially_destructible_v<T>)
                {
                    ptr->~T();
                }
                try
                {
                    new (ptr) T(std::forward<Args>(args)...);
                }
                catch (...)
                {
                    ::operator delete(ptr); // 重要：释放裸内存
                    throw;
                }
            }
        }

        // 把 buffer 放回 struct 以利用 Cache Locality
        struct alignas(std::hardware_destructive_interference_size) ThreadLocalCache
        {
            std::vector<T *> ptrs;
            T *bulk_buffer[LocalBatchSize]; // Cache Friendly: 紧挨着 ptrs

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
                for (T *ptr : ptrs)
                    delete ptr;
            }
        };

        moodycamel::ConcurrentQueue<T *> global_queue_;
        std::atomic<bool> is_active_{false}; // 生命周期守卫

        static ThreadLocalCache &GetLocalCache()
        {
            static thread_local ThreadLocalCache local_cache_;
            return local_cache_;
        }
    };
}