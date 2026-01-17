#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

#include "aegis/core/actor.h"
#include "aegis/core/aoi_grid.h"
#include "aegis/core/message.h"
#include "aegis/core/playerActor.h"

// 引入 Protobuf 依赖 (前置声明)
namespace aegis::protocol
{
    class SCMoveNtf;
    class SCEnterViewNtf;
    class SCLeaveViewNtf;
}

namespace aegis::core
{

    // --- SceneActor ---
    class SceneActor : public Actor
    {
    public:
        SceneActor(float width, float height, float cellSize);
        ~SceneActor() = default;

        // 核心：处理收到的消息
        void handle_message(ActorMessage *msg) override;

    private:
        // 内部处理逻辑 (串行执行，无需加锁)
        void OnHandleEnter(SceneEnterMsg *msg);
        void OnHandleLeave(SceneLeaveMsg *msg);
        void OnHandleMove(SceneMoveMsg *msg);

        // 辅助函数
        PlayerActor *GetPlayer(uint64_t actorId) const;

        template <typename T>
        void SendPacket(uint64_t targetId, uint32_t msgId, const T &proto);

        void SendBuffer(uint64_t targetId, uint32_t msgId, const std::string &buffer);

    private:
        AOIGrid aoi_;
        // 改为持有 shared_ptr，保证玩家生命周期安全
        std::unordered_map<uint64_t, PlayerActor *> actors_;

        // 缓存复用 (在 Actor 模型下单线程访问，安全)
        std::vector<uint64_t> cachedEnterIds_;
        std::vector<uint64_t> cachedLeaveIds_;
    };

} // namespace aegis::core