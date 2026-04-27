#pragma once
#include <cstdint>

namespace aegis::core
{
    // 【关键修复】：将底层类型从 uint8_t 升级到 uint16_t
    enum MessageType : uint16_t
    {
        MSG_TYPE_BASE = 0,
        MSG_TYPE_NETWORK = 1,
        MSG_TYPE_CORO_WAKEUP = 2,
        MSG_TYPE_SESSION_CLOSED = 3,

        MSG_TYPE_SCENE_ENTER = 10,
        MSG_TYPE_SCENE_LEAVE = 11,
        MSG_TYPE_SCENE_MOVE = 12,
        MSG_TYPE_SCENE_SKILL_CAST = 13,
        MSG_TYPE_REQ_SCENE_SNAPSHOT = 14,

        MSG_TYPE_DESTROY = 20,
        MSG_TYPE_ACTOR_DIED = 21,
        MSG_TYPE_POISON_PILL = 22,

        // RPCs (Module C)
        MSG_TYPE_RPC_CREATE_ROOM = 50,
        MSG_TYPE_RPC_TERMINATE_ROOM = 51,
        MSG_TYPE_RPC_ASSIGN_CAMP = 52,  // 玩家请求分配/创建营地

        MSG_TYPE_FORWARD_PACKET = 60,
        MSG_TYPE_REBIND_CONNECTION = 61,

        // RPC 响应（协程模式）
        MSG_TYPE_RPC_RESPONSE = 70,

        // RoomManager <-> SceneActor 人数同步
        MSG_TYPE_CAMP_PLAYER_COUNT = 71
    };
}
