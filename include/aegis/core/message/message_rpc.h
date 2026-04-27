#pragma once
#include <future>
#include <string>
#include <cstdint>
#include <functional>
#include "aegis/core/message/message_base.h"
#include "aegis/core/message/message_id.h"
#include "aegis/core/actor_registry.h"
#include "aegis/core/actor.h"
#include "aegis/common/actor_utils.h"
#include "aegis/common/aegisLog.h"
#include "ss_bridge.pb.h"

namespace aegis::core
{
    // ── RPC ID 统一类型 ──
    using RpcId = uint64_t;

    // ── RPC 响应消息（协程模式下由 Reply() 投递） ──
    struct RpcResponseMsg : public BasicMessage<RpcResponseMsg, MSG_TYPE_RPC_RESPONSE>
    {
        RpcId rpc_id = 0;
        void *result_storage = nullptr;
        std::function<void()> deleter;

        RpcResponseMsg(RpcId id, void *storage, std::function<void()> del)
            : rpc_id(id), result_storage(storage), deleter(std::move(del))
        {
        }

        void finalize()
        {
            if (deleter)
                deleter();
            delete this;
        }
    };

    // RPC 响应类型：控制 Reply() 的行为
    enum class RpcReplyMode : uint8_t
    {
        Promise,  // 旧模式：通过 std::promise 回复（同步阻塞用）
        Coroutine // 新模式：通过 dispatch_msg 投递 RpcResponseMsg（协程用）
    };

    // 【关键修复】：模板参数 TypeId 改为 uint16_t
    template <typename ReqT, typename ResT, uint16_t TypeId>
    struct RpcMessage : public ActorMessage
    {
        ReqT req;
        mutable std::promise<ResT> promise;
        mutable uint64_t rpc_id = 0;                     // 协程模式下的全局 RPC ID
        RpcReplyMode reply_mode = RpcReplyMode::Promise; // 回复模式
        ActorID requester_id_;                           // 协程模式下的发起者 ActorID（由 RpcAwaiter 设置）

        explicit RpcMessage(const ReqT &request) : req(request) { type_id = TypeId; }
        RpcMessage() { type_id = TypeId; }

        RpcMessage(const RpcMessage &) = delete;
        RpcMessage &operator=(const RpcMessage &) = delete;

        void finalize() { delete this; }

        // 设置 RPC 请求元数据（由 RpcAwaiter 在发送前调用）
        void set_rpc_meta(uint64_t id, ActorID requester)
        {
            rpc_id = id;
            requester_id_ = requester;
            reply_mode = RpcReplyMode::Coroutine;
        }

        // 由 RpcAwaiter 在 await_suspend 中更新真实 rpc_id
        void set_rpc_id(uint64_t id) const { rpc_id = id; }

        void Reply(const ResT &res) const
        {
            if (reply_mode == RpcReplyMode::Coroutine && rpc_id != 0)
            {
                // 新路径：投递 RpcResponseMsg 到发起者所在 Worker
                auto *storage = new ResT(res);
                auto *response = new RpcResponseMsg(
                    rpc_id, storage,
                    [storage]()
                    { delete storage; });

                // 找到发起者 Actor
                auto rid = requester_id_;
                if (rid.is_valid())
                {
                    auto *requester = ActorRegistry::instance().get(rid);
                    if (requester)
                    {
                        dispatch_msg(requester, response);
                        return;
                    }
                }

                // 如果找不到发起者，记录日志并清理
                aegis::Log::instance().warn("[RpcMessage] Requester {} not found, dropping RPC response",
                                            rid.raw);
                response->finalize();
                return;
            }

            // 旧路径：通过 promise 回复（阻塞模式）
            promise.set_value(res);
        }
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
        uint64_t player_uid = 0;      // 请求者的 UID
        ActorID player_actor_id;      // 请求者的 ActorID
        std::string camp_name;        // 营地名 (创建时使用)
        bool is_create = false;       // true = 创建新营地, false = 加入已有营地
        uint64_t target_scene_id = 0; // 加入目标营地 (is_create=false 时有效)
    };

    // AssignCampRes: RoomManager → PlayerActor 回复分配结果
    struct AssignCampRes
    {
        int32_t ret_code = 0;
        ActorID scene_actor_id; // 分配到的 SceneActor ID
        std::string camp_name;
        std::string err_msg;
    };

    using RPCAssignCampMsg = RpcMessage<
        AssignCampReq,
        AssignCampRes,
        MSG_TYPE_RPC_ASSIGN_CAMP>;
}
