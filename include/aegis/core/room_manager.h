#pragma once

#include <unordered_map>
#include "aegis/core/actor_traits.h" // 包含 SimpleActor
#include "aegis/core/message.h"
#include "aegis/core/actor_registry.h"
#include "cs_battle.pb.h" // 你的 Protobuf 定义

namespace aegis::core
{
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
        // --- 业务逻辑 ---
        void on_create_room(const RPCCreateRoomMsg &msg);
        void on_terminate_room(const RPCTerminateRoomMsg &msg);

        // --- 监管逻辑 ---
        void on_scene_died(uint64_t deceased_id, int reason);

    private:
        // 双向索引：保证 O(1) 的查找和清理
        std::unordered_map<uint32_t, uint32_t> room_id_to_actor_; // RoomID -> ActorID (Scene)
        std::unordered_map<uint32_t, uint32_t> actor_to_room_id_; // ActorID -> RoomID
    };
}