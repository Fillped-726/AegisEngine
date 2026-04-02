#include "aegis/net/packet_builder.h"
#include "cs_battle.pb.h" // 仅在此处暴露 Protobuf 依赖
#include "aegis/common/aegisLog.h"

namespace aegis::net
{
    using namespace aegis::cs::battle;

    std::shared_ptr<std::string> PacketBuilder::BuildEnterView(const EntityViewInfo &entity)
    {
        SCEnterViewNtf ntf;
        auto *ent = ntf.add_entities();
        ent->set_entity_id(entity.uid);
        ent->mutable_pos()->set_x(entity.x);
        ent->mutable_pos()->set_y(entity.y);
        return std::make_shared<std::string>(ntf.SerializeAsString());
    }

    std::shared_ptr<std::string> PacketBuilder::BuildEnterView(std::span<const EntityViewInfo> entities)
    {
        SCEnterViewNtf ntf;
        for (const auto &entity : entities)
        {
            auto *ent = ntf.add_entities();
            ent->set_entity_id(entity.uid);
            ent->mutable_pos()->set_x(entity.x);
            ent->mutable_pos()->set_y(entity.y);
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