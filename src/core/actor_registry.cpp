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

        if (!ptr) [[likely]]
        {
            return nullptr;
        }

        if (ptr->id().raw != id.raw)
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

        if (!ptr || ptr->id().raw != id.raw)
        {
            // Log 这里的 id.raw 是没问题的，因为它是 uint64
            aegis::Log::instance().warn("Attempted to remove non-existent or ID-mismatched Actor: {}", id.raw);
            return;
        }

        // 原子置空
        Actor *expected = ptr;
        if (actors_[idx].compare_exchange_strong(expected, nullptr, std::memory_order_release))
        {
            // 版本号自增
            versions_[idx].fetch_add(1, std::memory_order_relaxed);
            free_indices_.enqueue(idx);

            aegis::Log::instance().debug("Actor {} removed from registry.", id.raw);
        }
    }
}