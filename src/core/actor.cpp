#include "aegis/core/actor.h"

namespace aegis::core
{
    // [Core] 线程局部存储
    // 每个 Worker 线程都会有自己的一份 t_current_actor
    static thread_local Actor *t_current_actor = nullptr;

    Actor *Actor::current()
    {
        return t_current_actor;
    }

    void Actor::set_current(Actor *actor)
    {
        t_current_actor = actor;
    }

} // namespace aegis::core