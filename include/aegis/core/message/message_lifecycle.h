#pragma once
#include <coroutine>
#include "aegis/core/message/message_base.h"
#include "aegis/core/actor_registry.h"

namespace aegis::core
{
    // 协程唤醒消息
    struct CoroutineWakeupMsg : public BasicMessage<CoroutineWakeupMsg, MSG_TYPE_CORO_WAKEUP>
    {
        std::coroutine_handle<> handle;
        explicit CoroutineWakeupMsg(std::coroutine_handle<> h) : handle(h) {}
    };

    // 销毁消息
    struct ActorDestroyMsg : public BasicMessage<ActorDestroyMsg, MSG_TYPE_DESTROY>
    {
    };

    // 遗言消息
    struct ActorDiedMsg : public BasicMessage<ActorDiedMsg, MSG_TYPE_ACTOR_DIED>
    {
        core::ActorID deceased_id;
        int reason;
        ActorDiedMsg(core::ActorID id, int r = 0) : deceased_id(id), reason(r) {}
    };

    // [新增] 毒药丸消息 (真死兜底)
    struct PoisonPillMsg : public BasicMessage<PoisonPillMsg, MSG_TYPE_POISON_PILL>
    {
    };
}