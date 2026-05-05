/**
 * @file rpc_message.h
 * @brief RPC message types — extracted from core/message/message_rpc.h.
 *
 * Namespace migrated from aegis::core → aegis::rpc.
 * Backward-compatible using declarations are in the original header wrapper.
 */
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

namespace aegis::rpc
{
    // ── RPC ID 统一类型 ──
    using RpcId = uint64_t;

    // ── RPC 响应消息（协程模式下由 Reply() 投递） ──
    struct RpcResponseMsg : public core::BasicMessage<RpcResponseMsg, core::MSG_TYPE_RPC_RESPONSE>
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
    struct RpcMessage : public core::ActorMessage
    {
        ReqT req;
        mutable std::promise<ResT> promise;
        mutable uint64_t rpc_id = 0;                     // 协程模式下的全局 RPC ID
        RpcReplyMode reply_mode = RpcReplyMode::Promise; // 回复模式
        core::ActorID requester_id_;                     // 协程模式下的发起者 ActorID（由 RpcAwaiter 设置）

        explicit RpcMessage(const ReqT &request) : req(request) { type_id = TypeId; }
        RpcMessage() { type_id = TypeId; }

        RpcMessage(const RpcMessage &) = delete;
        RpcMessage &operator=(const RpcMessage &) = delete;

        void finalize() { delete this; }

        // 设置 RPC 请求元数据（由 RpcAwaiter 在发送前调用）
        void set_rpc_meta(uint64_t id, core::ActorID requester)
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
                    auto *requester = core::ActorRegistry::instance().get(rid);
                    if (requester)
                    {
                        core::dispatch_msg(requester, static_cast<core::ActorMessage *>(response));
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
        core::MSG_TYPE_RPC_CREATE_ROOM>;

    using RPCTerminateRoomMsg = RpcMessage<
        aegis::ss::bridge::TerminateRoomReq,
        aegis::ss::bridge::TerminateRoomRes,
        core::MSG_TYPE_RPC_TERMINATE_ROOM>;

    // ==========================================================
    // 内部营地分配 RPC (Module C)
    // ==========================================================

    // AssignCampReq: PlayerActor → RoomManager 请求分配一个营地
    struct AssignCampReq
    {
        uint64_t player_uid = 0;      // 请求者的 UID
        core::ActorID player_actor_id;      // 请求者的 ActorID
        std::string camp_name;        // 营地名 (创建时使用)
        bool is_create = false;       // true = 创建新营地, false = 加入已有营地
        uint64_t target_scene_id = 0; // 加入目标营地 (is_create=false 时有效)
    };

    // AssignCampRes: RoomManager → PlayerActor 回复分配结果
    struct AssignCampRes
    {
        int32_t ret_code = 0;
        core::ActorID scene_actor_id; // 分配到的 SceneActor ID
        std::string camp_name;
        std::string err_msg;
    };

    using RPCAssignCampMsg = RpcMessage<
        AssignCampReq,
        AssignCampRes,
        core::MSG_TYPE_RPC_ASSIGN_CAMP>;

    // ==========================================================
    // 副本 RPC (Dungeon)
    // ==========================================================

    // CreateDungeonReq: PlayerActor → RoomManager 请求创建副本
    struct CreateDungeonReq
    {
        uint64_t player_uid = 0;
        core::ActorID player_actor_id;
        uint32_t map_id = 1;
    };

    // CreateDungeonRes: RoomManager → PlayerActor 回复
    struct CreateDungeonRes
    {
        int32_t ret_code = 0;
        core::ActorID dungeon_scene_id; // 副本 SceneActor ID
        std::string err_msg;
    };

    using RPCCreateDungeonMsg = RpcMessage<
        CreateDungeonReq,
        CreateDungeonRes,
        core::MSG_TYPE_RPC_CREATE_DUNGEON>;

    // JoinDungeonReq: PlayerActor → RoomManager 请求加入副本
    struct JoinDungeonReq
    {
        uint64_t player_uid = 0;
        core::ActorID player_actor_id;
    };

    struct JoinDungeonRes
    {
        int32_t ret_code = 0;
        core::ActorID dungeon_scene_id;
        std::string err_msg;
    };

    using RPCJoinDungeonMsg = RpcMessage<
        JoinDungeonReq,
        JoinDungeonRes,
        core::MSG_TYPE_RPC_JOIN_DUNGEON>;

    // LeaveDungeonReq: SceneActor → RoomManager 玩家离开副本
    struct LeaveDungeonReq
    {
        uint64_t player_uid = 0;
        core::ActorID player_actor_id;
        bool is_owner = false; // 房主离开 = 副本销毁
    };

    struct LeaveDungeonRes
    {
        int32_t ret_code = 0;
        std::string err_msg;
    };

    using RPCLeaveDungeonMsg = RpcMessage<
        LeaveDungeonReq,
        LeaveDungeonRes,
        core::MSG_TYPE_RPC_LEAVE_DUNGEON>;
}
