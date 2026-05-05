#include <gtest/gtest.h>
#include "aegis/game/sync_manager.h"
#include "aegis/game/aoi_grid.h"
#include "aegis/game/playerActor.h"
#include "aegis/core/actor_registry.h"
#include "aegis/core/actor_traits.h"
#include <memory>
#include <string>
#include <vector>

using namespace aegis::core;

// ── 测试工厂 ──
// 不依赖 SceneActor，直接操作 SyncManager + AOIGrid + PlayerActor

// 记录广播事件
struct BroadcastEvent
{
    uint64_t target_id;
    std::string data;
};
static std::vector<BroadcastEvent> g_broadcastEvents;

class SyncManagerDirectTest : public ::testing::Test
{
protected:
    SyncManager sync_mgr_;
    AOIGrid *aoi_ = nullptr;
    std::vector<ActorID> actor_ids_;

    static constexpr float kMapWidth = 500.0f;
    static constexpr float kMapHeight = 500.0f;
    static constexpr float kCellSize = 10.0f;

    void SetUp() override
    {
        // 初始化 AOI 网格
        aoi_ = new AOIGrid(0.0f, 0.0f, kMapWidth, kMapHeight, kCellSize);
        g_broadcastEvents.clear();
    }

    void TearDown() override
    {
        for (auto &id : actor_ids_)
        {
            if (id.is_valid())
                ActorRegistry::instance().remove(id);
        }
        actor_ids_.clear();
        delete aoi_;
        aoi_ = nullptr;
    }

    // 创建一个带 AOI 注册的测试玩家
    ActorID CreatePlayer(uint64_t uid, float x, float y)
    {
        ActorID pid = ActorRegistry::instance().create_actor<PlayerActor>(nullptr);
        EXPECT_TRUE(pid.is_valid());
        actor_ids_.push_back(pid);

        auto *player = static_cast<PlayerActor *>(ActorRegistry::instance().get(pid));
        // 不设置 player_id，统一使用 ActorID (player->id().raw)
        player->SetPos(x, y);

        // 注册到 AOI 网格
        uint32_t gridIndex = aoi_->Add(pid.raw, x, y);
        player->set_aoi_grid_index(gridIndex);

        return pid;
    }

    // 移动玩家（更新坐标 + 脏标记）
    void MovePlayer(ActorID pid, float newX, float newY, float speed = 200.0f, bool isMoving = true)
    {
        auto *player = static_cast<PlayerActor *>(ActorRegistry::instance().get(pid));
        ASSERT_NE(player, nullptr);
        player->SetPos(newX, newY);
        player->SetMoveState(speed, isMoving);
        sync_mgr_.AddDirtyPlayer(player);
    }

    // 触发 Tick
    void TriggerTick()
    {
        g_broadcastEvents.clear();
        sync_mgr_.Tick(
            *aoi_,
            // onEnterLeave
            [](PlayerActor *mover,
               const std::vector<uint64_t> &enterIds,
               const std::vector<uint64_t> &leaveIds) {
                // 单元测试暂不处理 enter/leave
            },
            // onSendBatch
            [](uint64_t targetActorId, std::shared_ptr<std::string> sharedBuf) {
                g_broadcastEvents.push_back({targetActorId, *sharedBuf});
            });
    }

    // 获取某个 Actor 收到的广播次数
    int EventCountFor(uint64_t actorRaw) const
    {
        int count = 0;
        for (auto &e : g_broadcastEvents)
            if (e.target_id == actorRaw)
                count++;
        return count;
    }
};

// ================================================================
// 测试 1: 单玩家移动 → 无广播
// ================================================================
TEST_F(SyncManagerDirectTest, SinglePlayerNoBroadcast)
{
    auto p1 = CreatePlayer(1001, 50.0f, 50.0f);
    MovePlayer(p1, 55.0f, 50.0f);
    TriggerTick();

    EXPECT_EQ(EventCountFor(p1.raw), 0)
        << "单玩家移动不应产生广播";
}

// ================================================================
// 测试 2: 两个玩家在九宫格内 → 移动广播给邻居
// ================================================================
TEST_F(SyncManagerDirectTest, TwoPlayersBroadcast)
{
    auto p1 = CreatePlayer(1001, 50.0f, 50.0f); // grid (5,5)
    auto p2 = CreatePlayer(1002, 60.0f, 60.0f); // grid (6,6) — 同一 9 宫格

    MovePlayer(p1, 55.0f, 55.0f);
    TriggerTick();

    EXPECT_GT(EventCountFor(p2.raw), 0)
        << "p2 在 p1 视野内应收到广播";
    EXPECT_EQ(EventCountFor(p1.raw), 0)
        << "p1 不应收到自己的广播";
}

// ================================================================
// 测试 3: 超出视野 → 不广播
// ================================================================
TEST_F(SyncManagerDirectTest, OutOfRangeNoBroadcast)
{
    auto p1 = CreatePlayer(1001, 50.0f, 50.0f);   // grid (5,5)
    auto p2 = CreatePlayer(1002, 450.0f, 450.0f); // grid (45,45)

    MovePlayer(p1, 55.0f, 50.0f);
    TriggerTick();

    EXPECT_EQ(EventCountFor(p2.raw), 0)
        << "超出 AOI 视野的玩家不应收到广播";
}

// ================================================================
// 测试 4: 连续不跨格移动 → 持续广播坐标
// ================================================================
TEST_F(SyncManagerDirectTest, ContinuousMovementInSameGrid)
{
    auto p1 = CreatePlayer(1001, 50.0f, 50.0f);
    auto p2 = CreatePlayer(1002, 60.0f, 60.0f); // grid (6,6)

    // p1 在同一个格子里移动 3 次
    for (int i = 0; i < 3; i++)
    {
        MovePlayer(p1, 52.0f + i * 1.0f, 50.0f);
        TriggerTick();
    }

    // p2 至少收到一次广播
    EXPECT_GT(EventCountFor(p2.raw), 0)
        << "不跨格移动也应广播坐标更新";
}

// ================================================================
// 测试 5: 跨格触发 AOI Enter/Leave + 继续广播
// ================================================================
TEST_F(SyncManagerDirectTest, GridCrossAndContinue)
{
    auto p1 = CreatePlayer(1001, 50.0f, 50.0f); // grid (5,5)
    auto p2 = CreatePlayer(1002, 15.0f, 15.0f); // grid (1,1)

    // p1 跨格到 (2,2) 附近
    MovePlayer(p1, 25.0f, 25.0f);
    TriggerTick();

    EXPECT_GT(EventCountFor(p2.raw), 0)
        << "跨格后 p1 应进入 p2 的 AOI 视野";

    // 在同格内继续移动
    MovePlayer(p1, 28.0f, 28.0f);
    TriggerTick();

    int eventsAfterSecond = EventCountFor(p2.raw);
    EXPECT_GE(eventsAfterSecond, 1)
        << "跨格后继续移动，p2 应持续收到广播";
}

// ================================================================
// 测试 6: 两个玩家都在动 → 互相收到广播
// ================================================================
TEST_F(SyncManagerDirectTest, BothMovingMutualBroadcast)
{
    auto p1 = CreatePlayer(1001, 50.0f, 50.0f);
    auto p2 = CreatePlayer(1002, 60.0f, 60.0f); // grid (6,6)

    // 两人同时移动（靠近，保持在 9 宫格内）
    MovePlayer(p1, 55.0f, 55.0f);
    MovePlayer(p2, 55.0f, 60.0f);
    TriggerTick();

    EXPECT_GT(EventCountFor(p1.raw), 0)
        << "p1 应收到 p2 的广播";
    EXPECT_GT(EventCountFor(p2.raw), 0)
        << "p2 应收到 p1 的广播";
}

// ================================================================
// 测试 7: 速度/移动状态通过 Tick 传递（验证速度字段）
// ================================================================
TEST_F(SyncManagerDirectTest, SpeedAndMovingInBroadcast)
{
    auto p1 = CreatePlayer(1001, 50.0f, 50.0f);
    auto p2 = CreatePlayer(1002, 60.0f, 60.0f); // grid (6,6)

    MovePlayer(p1, 55.0f, 55.0f, 200.0f, true);
    TriggerTick();

    ASSERT_GT(EventCountFor(p2.raw), 0);

    // 解析 protobuf 验证 speed/is_moving
    ASSERT_FALSE(g_broadcastEvents.empty());
    for (auto &evt : g_broadcastEvents)
    {
        if (evt.target_id == p2.raw)
        {
            aegis::cs::battle::SCMoveNtfBatch batch;
            ASSERT_TRUE(batch.ParseFromString(evt.data));
            ASSERT_GT(batch.moves_size(), 0);
            auto &move = batch.moves(0);
            EXPECT_FLOAT_EQ(move.speed(), 200.0f);
            EXPECT_TRUE(move.is_moving());
            return;
        }
    }
    FAIL() << "没有找到发给 p2 的广播";
}
