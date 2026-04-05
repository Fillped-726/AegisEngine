// dispatcher.h
#pragma once

#include <functional>
#include <unordered_map>
#include <memory>
#include <string>
#include <type_traits>
#include <concepts>

#include "aegis/core/task.h"       // [DEPENDENCY: aegis::core::Task]
#include "aegis/core/actor.h"      // [DEPENDENCY: aegis::core::Actor]
#include "aegis/net/packet.h"      // [DEPENDENCY: aegis::net::Packet]
#include "aegis/common/aegisLog.h" // [DEPENDENCY: aegis::Log]

namespace aegis::net
{
    // [CONSTRAINT: Structural conformance to Protobuf ParseFromArray]
    template <typename T>
    concept ProtobufMessage = requires(T t, const void *data, int len) {
        { t.ParseFromArray(data, len) } -> std::convertible_to<bool>;
    };

    // [INTENT: Singleton async message router for Net/Protobuf and Local/RPC payloads]
    class Dispatcher
    {
    public:
        // [STATE: Type-erased coroutine wrapper for raw byte streams]
        using NetHandler = std::function<core::Task<void>(core::Actor *, const char *, size_t)>;

        // [STATE: Type-erased coroutine wrapper for in-memory object pointers]
        using RpcHandler = std::function<core::Task<void>(core::Actor *, const void *)>;

        static Dispatcher &instance()
        {
            static Dispatcher inst;
            return inst;
        }

        // [STATE_MUTATION: Inject deserializing closure into Net routing table]
        template <typename ProtoMsg, typename Func>
            requires ProtobufMessage<ProtoMsg>
        void register_handler(uint32_t msg_id, Func &&func)
        {
            NetHandler wrapper = [func = std::forward<Func>(func), msg_id](core::Actor *actor, const char *data, size_t len) -> core::Task<void>
            {
                ProtoMsg msg;
                // [INTENT: Hydrate Protobuf object before dispatch]
                if (!msg.ParseFromArray(data, static_cast<int>(len)))
                {
                    aegis::Log::instance().warn("Dispatcher: Parse failed. MsgID: {}", msg_id);
                    co_return;
                }
                co_await func(actor, msg);
            };

            net_handlers_[msg_id] = std::move(wrapper);
        }

        // [STATE_MUTATION: Inject zero-copy static_cast closure into RPC routing table]
        template <typename RpcMsgType, typename Func>
        void register_rpc(uint32_t msg_id, Func &&func)
        {
            RpcHandler wrapper = [func = std::forward<Func>(func)](core::Actor *actor, const void *msg_ptr) -> core::Task<void>
            {
                // [INTENT: Safe downcast reliant on msg_id correlation]
                const auto *msg = static_cast<const RpcMsgType *>(msg_ptr);
                co_await func(actor, *msg);
            };

            rpc_handlers_[msg_id] = std::move(wrapper);
        }

        // [INTENT: Demultiplex raw network packet by msg_id]
        core::Task<void> dispatch(core::Actor *actor, const Packet &pkt)
        {
            uint32_t msg_id = pkt.msg_id();
            auto it = net_handlers_.find(msg_id);
            if (it != net_handlers_.end())
            {
                co_await it->second(actor, static_cast<const char *>(pkt.data() + kPacketMsgHeader), pkt.size() - kPacketMsgHeader);
            }
            else
            {
                aegis::Log::instance().warn("Dispatcher: No NetHandler for MsgID: {}", msg_id);
            }
        }

        // [INTENT: Demultiplex type-erased RPC pointer by msg_id]
        core::Task<void> dispatch_rpc(core::Actor *actor, uint32_t msg_id, const void *msg_ptr)
        {
            auto it = rpc_handlers_.find(msg_id);
            if (it != rpc_handlers_.end())
            {
                co_await it->second(actor, msg_ptr);
            }
            else
            {
                aegis::Log::instance().error("Dispatcher: No RpcHandler for MsgID: {}", msg_id);
            }
        }

    private:
        Dispatcher() = default;

        // [STATE: O(1) routing tables]
        std::unordered_map<uint32_t, NetHandler> net_handlers_;
        std::unordered_map<uint32_t, RpcHandler> rpc_handlers_;
    };

} // namespace aegis::net