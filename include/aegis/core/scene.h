#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

#include "aegis/core/aoi_grid.h"
#include "aegis/common/aegisLog.h"

#include "aegis/core/playerActor.h"

namespace aegis::protocol
{
    class SCMoveNtf;
    class SCEnterViewNtf;
    class SCLeaveViewNtf;
}

namespace aegis::core
{
    class Scene
    {
    public:
        Scene(float width, float height, float cellSize);
        ~Scene() = default;

        Scene(const Scene &) = delete;
        Scene &operator=(const Scene &) = delete;

        void AddPlayer(PlayerActor *actor);
        void RemovePlayer(uint64_t actorId);
        PlayerActor *GetPlayer(uint64_t actorId) const;

        void OnPlayerMove(PlayerActor *mover, float newX, float newY);

    private:
        template <typename T>
        void SendPacket(uint64_t targetId, uint32_t msgId, const T &proto);

        void SendBuffer(uint64_t targetId, uint32_t msgId, const std::string &buffer);

    private:
        AOIGrid aoi_;
        std::unordered_map<uint64_t, PlayerActor *> actors_;

        // 缓存复用
        std::vector<uint64_t> cachedEnterIds_;
        std::vector<uint64_t> cachedLeaveIds_;
    };

} // namespace aegis::core