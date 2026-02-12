#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <memory>
#include <cstdint>
#include <type_traits>
#include <string>

#include "concurrentqueue.h"
#include "aegis/common/aegisLog.h"

namespace aegis::core
{
    class Actor;
    /**
     * @brief 复合 ID 定义 (Handle)
     * 占用 4 字节，包含 16 位下标和 16 位版本号，用于解决 ABA 问题
     */
    union ActorID
    {
        struct
        {
            uint32_t index;   ///< 数组下标 (0 ~ 65535)
            uint32_t version; ///< 代数 (每次复用 +1)
        } parts;
        uint64_t raw; ///< 用于高效比较和网络传输

        ActorID() : raw(0) {}
        explicit ActorID(uint32_t val) : raw(val) {}

        bool operator==(const ActorID &other) const { return raw == other.raw; }
        bool operator!=(const ActorID &other) const { return raw != other.raw; }

        bool is_valid() const { return raw != 0; }
        std::string to_string() const { return std::to_string(raw); }
    };

    /**
     * @brief Actor 注册中心 (Registry)
     * 管理全局 Actor 的生命周期映射，提供基于 ID 的 O(1) 无锁查询
     */
    class ActorRegistry
    {
    public:
        static constexpr size_t MAX_ACTORS = 65536;
        static ActorRegistry &instance();
        // 禁止拷贝
        ActorRegistry(const ActorRegistry &) = delete;
        ActorRegistry &operator=(const ActorRegistry &) = delete;

        /**
         * @brief 模板化创建 Actor 并注册
         * @return 成功返回唯一 ActorID，失败返回 raw=0 的 ID
         */
        template <typename T, typename... Args>
        ActorID create_actor(Args &&...args)
        {
            static_assert(std::is_base_of<Actor, T>::value, "T must derive from Actor");

            // -----------------------------------------------------------
            // 1. 先申请 Slot (占座)
            // -----------------------------------------------------------
            uint32_t idx;
            if (!free_indices_.try_dequeue(idx))
            {
                aegis::Log::instance().error("ActorRegistry overflow! Max actors ({}) reached.", MAX_ACTORS);
                return ActorID(0);
            }

            // -----------------------------------------------------------
            // 2. 立即生成完整的 ActorID (确立身份)
            // -----------------------------------------------------------
            uint32_t ver = versions_[idx].load(std::memory_order_relaxed);

            ActorID id;
            id.parts.index = idx;
            id.parts.version = ver;

            // -----------------------------------------------------------
            // 3. 带着 ID 去内存分配与构造 (出生)
            // -----------------------------------------------------------
            T *actor = nullptr;
            try
            {
                // [关键修改] 将 id 作为第一个参数传给 create
                // 这要求 PooledActor::create 和 SimpleActor::create 必须接受 id
                actor = T::create(id, std::forward<Args>(args)...);
            }
            catch (const std::exception &e)
            {
                aegis::Log::instance().error("Exception during Actor creation at index {}: {}", idx, e.what());
                free_indices_.enqueue(idx); // 回滚 Slot
                throw;
            }
            catch (...)
            {
                aegis::Log::instance().error("Unknown exception during Actor creation at index {}", idx);
                free_indices_.enqueue(idx); // 回滚 Slot
                throw;
            }

            if (!actor)
            {
                aegis::Log::instance().error("Actor::create returned nullptr at index {}", idx);
                free_indices_.enqueue(idx);
                return ActorID(0);
            }

            // 5. 发布 (Release 语义保证初始化对 Get 线程可见)
            actors_[idx].store(actor, std::memory_order_release);

            aegis::Log::instance().debug("Actor created. ID: {}, Type: {}", id.raw, typeid(T).name());
            return id;
        }

        /**
         * @brief 路由查询 (Hot Path)
         */
        Actor *get(ActorID id);

        Actor *get(uint32_t raw_id)
        {
            return get(ActorID(raw_id));
        }

        /**
         * @brief 逻辑销毁
         * 将 Actor 从注册表中移除并回收 Index，不负责内存释放
         */
        void remove(ActorID id);

    private:
        ActorRegistry()
        {
            for (auto &v : versions_)
                v.store(1, std::memory_order_relaxed);

            for (auto &p : actors_)
                p.store(nullptr, std::memory_order_relaxed);

            for (uint32_t i = 0; i < MAX_ACTORS; ++i)
                free_indices_.enqueue(static_cast<uint16_t>(i));

            aegis::Log::instance().info("ActorRegistry initialized. Max capacity: {}", MAX_ACTORS);
        }

        // --- 核心存储 ---
        std::array<std::atomic<Actor *>, MAX_ACTORS> actors_;
        std::array<std::atomic<uint16_t>, MAX_ACTORS> versions_;
        moodycamel::ConcurrentQueue<uint16_t> free_indices_;
    };

} // namespace aegis::core