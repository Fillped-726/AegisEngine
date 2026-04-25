/**
 * @file objectPool.h
 * @brief TLS-cached object pool with lock-free hot path.
 * 
 * Two-tier architecture: thread-local batch (lock-free) + global pool (ConcurrentQueue).
 */
#pragma once

#include <cstddef>
#include <utility>
#include <memory>
#include <vector>
#include <type_traits>
#include <concepts>
#include <new>
#include <atomic>

#include "concurrentqueue.h" // [DEPENDENCY: moodycamel::ConcurrentQueue]
#include "aegisLog.h"

namespace aegis::core
{
    // [CONSTRAINT: T implements reset(Args...)]
    template <typename T, typename... Args>
    concept Resettable = requires(T t, Args &&...args) {
        t.reset(std::forward<Args>(args)...);
    };

    // [INTENT: Lock-free 2-tier object pool (Thread-Local L1 + Global L2)]
    template <typename T, size_t GlobalMaxSize = 100000, size_t LocalBatchSize = 128>
    class ObjectPool
    {
    public:
        // [INTENT: Proxy functor for unique_ptr custom deleter]
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

        ObjectPool(const ObjectPool &) = delete;
        ObjectPool &operator=(const ObjectPool &) = delete;

        // [STATE_MUTATION: Extract from L1 -> L2 -> Heap; Reinitialize state]
        template <typename... Args>
        Ptr acquire(Args &&...args)
        {
            if (!is_active_.load(std::memory_order_acquire)) [[unlikely]]
            {
                return Ptr(new T(std::forward<Args>(args)...));
            }

            T *ptr = nullptr;
            auto &local = GetLocalCache();

            if (!local.ptrs.empty())
            {
                ptr = local.ptrs.back();
                local.ptrs.pop_back();
            }
            else
            {
                // [STATE_MUTATION: Bulk dequeue from L2 to L1 buffer]
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

        // [STATE_MUTATION: Return to L1 -> Evict half to L2 if L1 full -> Delete if L2 full]
        void release(T *ptr)
        {
            if (!ptr)
                return;

            if (!is_active_.load(std::memory_order_acquire)) [[unlikely]]
            {
                delete ptr;
                return;
            }

            auto &local = GetLocalCache();

            local.ptrs.push_back(ptr);

            if (local.ptrs.size() >= LocalBatchSize)
            {
                const size_t move_count = LocalBatchSize / 2;
                auto start_it = local.ptrs.end() - move_count;

                if (global_queue_.size_approx() < GlobalMaxSize)
                {
                    // [STATE_MUTATION: Bulk enqueue iterators to L2]
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
        // [STATE_MUTATION: Set guard active]
        ObjectPool() { is_active_.store(true, std::memory_order_release); }

        // [STATE_MUTATION: Set guard inactive; drain L2 queue]
        ~ObjectPool()
        {
            is_active_.store(false, std::memory_order_release);
            T *ptr;
            while (global_queue_.try_dequeue(ptr))
                delete ptr;
        }

        // [INTENT: Transparent state re-init; handles exception-safe placement new]
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
                    ::operator delete(ptr);
                    throw;
                }
            }
        }

        // [STATE: L1 Cache; Aligned to prevent false sharing cache line invalidation]
        struct alignas(std::hardware_destructive_interference_size) ThreadLocalCache
        {
            std::vector<T *> ptrs;
            T *bulk_buffer[LocalBatchSize];

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

        // [STATE: L2 Queue]
        moodycamel::ConcurrentQueue<T *> global_queue_;
        // [STATE: Process teardown guard]
        std::atomic<bool> is_active_{false};

        // [INTENT: Thread-local singleton accessor]
        static ThreadLocalCache &GetLocalCache()
        {
            static thread_local ThreadLocalCache local_cache_;
            return local_cache_;
        }
    };
}