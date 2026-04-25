/**
 * @file GameMessage.h
 * @brief Game-specific message type definitions (SceneSkillCast etc.).
 */
#pragma once
#include "aegis/core/message.h"

namespace aegis::core
{
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
