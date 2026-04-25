/**
 * @file room_manager.h
 * @brief Room manager for room lifecycle and camp assignment (Module C).
 *
 * Extends RPC handlers with on_assign_camp() — receives AssignCampReq
 * from PlayerActor, creates a new SceneActor camp or returns an existing one.
 */
#pragma once

#include <unordered_map>
#include "aegis/core/actor_traits.h" // 包含 SimpleActor
#include "aegis/core/message/message_rpc.h"
#include "aegis/core/actor_registry.h"
#include "cs_battle.pb.h" // 你的 Protobuf 定义

namespace aegis::core
{
    /**
     * @brief Room lifecycle and camp assignment manager.
     *
     * Creates and terminates game rooms (scenes), and handles
     * dynamic camp creation/joining for PlayerActors.
     */
    class RoomManager : public SimpleActor<RoomManager>
    {
    public:
        // 继承构造函数
        using SimpleActor::SimpleActor;

        ~RoomManager() override = default;

    protected:
        // 核心：消息分发中心
        void handle_message(ActorMessage *msg) override;

    private:
        // --- Room 生命周期 ---
        void on_create_room(const RPCCreateRoomMsg &msg);
        void on_terminate_room(const RPCTerminateRoomMsg &msg);

        // --- 监管逻辑 ---
        void on_scene_died(uint64_t deceased_id, int reason);

        // --- 营地分配 (Module C) ---
        void on_assign_camp(const RPCAssignCampMsg &msg);

    private:
        // 房间：RoomID <-> SceneActorID
        std::unordered_map<uint32_t, uint32_t> room_id_to_actor_; // RoomID -> ActorID (Scene)
        std::unordered_map<uint32_t, uint32_t> actor_to_room_id_; // ActorID -> RoomID

        // 营地：预创建的公共营地池 (可实现简单的负载分配)
        // 这里简化为: 创建新营地就存起来, 加入时遍历
        std::unordered_map<uint64_t, std::string> camp_scenes_;   // SceneActorID.raw -> camp_name
    };
}
