#pragma once
#include <atomic>
#include <concepts>
#include "aegis/core/message/message_id.h"

namespace aegis::core
{
    struct ActorMessage
    {
        std::atomic<ActorMessage *> next{nullptr};
        // 【关键修复】：类型同步改为 uint16_t
        uint16_t type_id = MSG_TYPE_BASE;

        virtual ~ActorMessage() = default;
    };

    using MessageFinalizer = void (*)(ActorMessage *);

    // 【扩容】：正式扩展为 1024
    extern MessageFinalizer g_message_finalizers[1024];

    // 【关键修复】：模板参数 TypeId 也必须是 uint16_t
    template <typename Derived, uint16_t TypeId>
    struct BasicMessage : public ActorMessage
    {
        BasicMessage()
        {
            type_id = TypeId;
            [[maybe_unused]] static bool registered = []()
            {
                g_message_finalizers[TypeId] = [](ActorMessage *msg)
                {
                    static_cast<Derived *>(msg)->finalize();
                };
                return true;
            }();
        }

        void finalize()
        {
            static_assert(sizeof(Derived) > 0, "Derived must be a complete type");
            delete static_cast<Derived *>(this);
        }
    };

    template <typename T>
    concept FinalizableMessage = std::derived_from<T, ActorMessage> && requires(T m) {
        { m.finalize() } -> std::same_as<void>;
    };
}