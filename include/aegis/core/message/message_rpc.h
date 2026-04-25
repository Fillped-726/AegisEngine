#pragma once
#include <future>
#include <string>
#include <cstdint>
#include "aegis/core/message/message_base.h"
#include "aegis/core/actor_registry.h"
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

    // ==========================================================
    // 内部营地分配 RPC (Module C)
    // ==========================================================

    // AssignCampReq: PlayerActor → RoomManager 请求分配一个营地
    struct AssignCampReq
    {
        uint64_t player_uid = 0;       // 请求者的 UID
        ActorID player_actor_id;       // 请求者的 ActorID
        std::string camp_name;         // 营地名 (创建时使用)
        bool is_create = false;        // true = 创建新营地, false = 加入已有营地
        uint64_t target_scene_id = 0;  // 加入目标营地 (is_create=false 时有效)
    };

    // AssignCampRes: RoomManager → PlayerActor 回复分配结果
    struct AssignCampRes
    {
        int32_t ret_code = 0;
        ActorID scene_actor_id;        // 分配到的 SceneActor ID
        std::string camp_name;
        std::string err_msg;
    };

    using RPCAssignCampMsg = RpcMessage<
        AssignCampReq,
        AssignCampRes,
        MSG_TYPE_RPC_ASSIGN_CAMP>;
}
