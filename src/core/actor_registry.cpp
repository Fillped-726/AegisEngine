#include "aegis/core/actor_registry.h"
#include "aegis/core/actor.h"
#include "aegis/common/aegisLog.h"

namespace aegis::core
{
    ActorRegistry &ActorRegistry::instance()
    {
        static ActorRegistry instance;
        return instance;
    }

    Actor *ActorRegistry::get(ActorID id)
    {
        uint32_t idx = id.parts.index;

        if (idx >= MAX_ACTORS) [[unlikely]]
        {
            return nullptr;
        }

        Actor *ptr = actors_[idx].load(std::memory_order_acquire);

        if (!ptr) [[unlikely]]
        {
            return nullptr;
        }

        uint32_t current_ver = versions_[idx].load(std::memory_order_relaxed);
        if (current_ver != id.parts.version) [[unlikely]]
        {
            return nullptr;
        }

        return ptr;
    }

    void ActorRegistry::remove(ActorID id)
    {
        uint32_t idx = id.parts.index;
        if (idx >= MAX_ACTORS)
            return;

        Actor *ptr = actors_[idx].load(std::memory_order_relaxed);

        uint32_t current_ver = versions_[idx].load(std::memory_order_relaxed);

        if (current_ver != id.parts.version)
        {
            aegis::Log::instance().warn("Attempted to remove ID-mismatched Actor: {}", id.raw);
            return;
        }

        Actor *expected = actors_[idx].load(std::memory_order_acquire);

        if (!expected)
        {
            aegis::Log::instance().warn("Attempted to remove non-existent Actor: {}", id.raw);
            return;
        }

        expected = ptr;
        if (actors_[idx].compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed))
        {
            // 版本号自增
            versions_[idx].fetch_add(1, std::memory_order_relaxed);
            free_indices_.enqueue(idx);

            aegis::Log::instance().debug("Actor {} removed from registry.", id.raw);
        }
    }

}