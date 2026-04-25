#pragma once
#include "aegis/core/message/message_base.h"
#include "aegis/core/actor_registry.h"

namespace aegis::core
{
    struct SceneEnterMsg : public BasicMessage<SceneEnterMsg, MSG_TYPE_SCENE_ENTER>
    {
        core::ActorID actor_id;
        uint64_t player_id;
        float x, y;
        SceneEnterMsg(core::ActorID pid, uint64_t uid, float px, float py) : actor_id(pid), player_id(uid), x(px), y(py) {}
    };

    struct SceneLeaveMsg : public BasicMessage<SceneLeaveMsg, MSG_TYPE_SCENE_LEAVE>
    {
        core::ActorID actor_id;
        uint64_t player_id;
        explicit SceneLeaveMsg(core::ActorID pid, uint64_t uid) : actor_id(pid), player_id(uid) {}
    };

    struct SceneMoveMsg : public BasicMessage<SceneMoveMsg, MSG_TYPE_SCENE_MOVE>
    {
        core::ActorID actor_id;
        uint64_t player_id;
        uint32_t aoi_grid_index;
        float newX, newY;
        uint8_t direction;

        SceneMoveMsg(core::ActorID id, uint64_t uid, uint32_t grid_index, float nx, float ny, uint8_t dir)
            : actor_id(id), player_id(uid), aoi_grid_index(grid_index), newX(nx), newY(ny), direction(dir) {}
    };

    // [新增] 向 SceneActor 请求快照
    struct ReqSceneSnapshotMsg : public BasicMessage<ReqSceneSnapshotMsg, MSG_TYPE_REQ_SCENE_SNAPSHOT>
    {
        uint64_t player_id; // 请求者 UID
        ReqSceneSnapshotMsg(uint64_t uid) : player_id(uid) {}
    };

    struct SceneSkillCastMsg : public BasicMessage<SceneSkillCastMsg, MSG_TYPE_SCENE_SKILL_CAST>
    {
        ActorID actor_id;    // 施法者 ID
        uint32_t skill_id;   // 技能 ID
        uint64_t target_uid; // 目标唯一 ID
        float target_x;      // 目标坐标 X
        float target_y;      // 目标坐标 Y

        // 构造函数：不再需要手动给基类传 TypeId，BasicMessage 内部已完成
        SceneSkillCastMsg(ActorID aid, uint32_t sid, uint64_t tuid, float tx, float ty)
            : actor_id(aid),
              skill_id(sid),
              target_uid(tuid),
              target_x(tx),
              target_y(ty)
        {
            // 此时 type_id 已经被 BasicMessage 的构造函数设为 MSG_TYPE_SCENE_SKILL_CAST
        }
    };
}