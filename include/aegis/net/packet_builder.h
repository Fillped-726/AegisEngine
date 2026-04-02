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