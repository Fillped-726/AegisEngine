#pragma once
#include <future>
#include "aegis/core/message/message_base.h"
#include "ss_bridge.pb.h"

namespace aegis::core
{
    // 【关键修复】：模板参数 TypeId 改为 uint16_t
    template <typename ReqT, typename ResT, uint16_t TypeId>
    struct RpcMessage : public ActorMessage
    {
        ReqT req;
        mutable std::promise<ResT> promise;

        explicit RpcMessage(const ReqT &request) : req(request) { type_id = TypeId; }
        RpcMessage() { type_id = TypeId; }

        RpcMessage(const RpcMessage &) = delete;
        RpcMessage &operator=(const RpcMessage &) = delete;

        void finalize() { delete this; }
        void Reply(const ResT &res) const { promise.set_value(res); }
    };

    using RPCCreateRoomMsg = RpcMessage<
        aegis::ss::bridge::CreateRoomReq,
        aegis::ss::bridge::CreateRoomRes,
        MSG_TYPE_RPC_CREATE_ROOM>;

    using RPCTerminateRoomMsg = RpcMessage<
        aegis::ss::bridge::TerminateRoomReq,
        aegis::ss::bridge::TerminateRoomRes,
        MSG_TYPE_RPC_TERMINATE_ROOM>;
}