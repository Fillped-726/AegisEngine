// packet_builder.cpp
#include "aegis/net/packet_builder.h"
#include "cs_battle.pb.h" // 仅在此处暴露 Protobuf 依赖
#include "common.pb.h"    // PBPlayerInfo
#include "aegis/common/aegisLog.h"

namespace aegis::net
{
    using namespace aegis::cs::battle;

    static void FillEntityProto(aegis::common::PBPlayerInfo *ent, const EntityViewInfo &entity)
    {
        ent->set_entity_id(entity.uid);
        ent->mutable_pos()->set_x(entity.x);
        ent->mutable_pos()->set_y(entity.y);
        ent->set_entity_type(entity.entity_type);
        ent->set_direction(entity.direction);
        ent->set_speed(entity.speed);
        ent->set_is_moving(entity.is_moving);
    }

    std::shared_ptr<std::string> PacketBuilder::BuildEnterView(const EntityViewInfo &entity)
    {
        SCEnterViewNtf ntf;
        FillEntityProto(ntf.add_entities(), entity);
        return std::make_shared<std::string>(ntf.SerializeAsString());
    }

    std::shared_ptr<std::string> PacketBuilder::BuildEnterView(std::span<const EntityViewInfo> entities)
    {
        SCEnterViewNtf ntf;
        for (const auto &entity : entities)
        {
            FillEntityProto(ntf.add_entities(), entity);
        }
        return std::make_shared<std::string>(ntf.SerializeAsString());
    }

    std::shared_ptr<std::string> PacketBuilder::BuildLeaveView(uint64_t uid)
    {
        SCLeaveViewNtf ntf;
        ntf.add_entity_ids(uid);
        return std::make_shared<std::string>(ntf.SerializeAsString());
    }

    std::shared_ptr<std::string> PacketBuilder::BuildLeaveView(std::span<const uint64_t> uids)
    {
        SCLeaveViewNtf ntf;
        for (uint64_t id : uids)
        {
            ntf.add_entity_ids(id);
        }
        return std::make_shared<std::string>(ntf.SerializeAsString());
    }

} // namespace aegis::network
