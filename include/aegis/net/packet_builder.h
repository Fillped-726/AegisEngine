/**
 * @file packet_builder.h
 * @brief Stateless factory for AOI enter/leave protobuf serialization.
 */
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <span> // [DEPENDENCY: C++20 std::span]

namespace aegis::net
{

    // [STATE: POD; spatial awareness payload for Area of Interest (AoI)]
    struct EntityViewInfo
    {
        uint64_t uid;
        float x;
        float y;
        float direction = 0.0f;    // 朝向角（弧度）
        float speed = 0.0f;        // 当前速度
        bool is_moving = false;    // 是否正在移动
        uint32_t entity_type = 0;  // 0: Player, 1: NPC, 2: Monster, 3: boss
    };

    // [INTENT: Stateless factory for Protobuf serialization; yields immutable shared network buffers for broadcast]
    class PacketBuilder
    {
    public:
        PacketBuilder() = delete;

        // [INTENT: Serialize single-entity AoI entrance]
        static std::shared_ptr<std::string> BuildEnterView(const EntityViewInfo &entity);

        // [INTENT: Serialize batch AoI entrance via contiguous memory view]
        static std::shared_ptr<std::string> BuildEnterView(std::span<const EntityViewInfo> entities);

        // [INTENT: Serialize single-entity AoI exit]
        static std::shared_ptr<std::string> BuildLeaveView(uint64_t uid);

        // [INTENT: Serialize batch AoI exit via contiguous memory view]
        static std::shared_ptr<std::string> BuildLeaveView(std::span<const uint64_t> uids);
    };

} // namespace aegis::network