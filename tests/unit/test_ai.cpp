#include <gtest/gtest.h>
#include "aegis/core/scene_actor.h"
#include "aegis/core/npc_actor.h"
#include "aegis/core/playerActor.h"
#include "aegis/core/GameMessage.h"
#include "aegis/core/actor_traits.h"

using namespace aegis::core;

class SceneAiTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 1. 初始化场景 (100x100 的地图，格子大小 10)
        // 假设视野是九宫格，那么视野距离大约是 10 左右
        scene_id_ = ActorRegistry::instance().create_actor<SceneActor>(100.0f, 100.0f, 10.0f);
        scene_ = static_cast<SceneActor *>(ActorRegistry::instance().get(scene_id_));
    }

    void TearDown() override
    {
        // 清理场景
        ActorRegistry::instance().remove(scene_id_);
    }

    ActorID scene_id_;
    SceneActor *scene_ = nullptr;
};

// ===================================================================
// KR2 测试：基于 AOI 的动态唤醒与休眠
// ===================================================================
TEST_F(SceneAiTest, NpcDynamicSleepTriggeredByAoi)
{
    // 1. 创建并放置 NPC 在地图中心 (50, 50)
    ActorID npc_id = ActorRegistry::instance().create_actor<NpcActor>();
    NpcActor *npc = static_cast<NpcActor *>(ActorRegistry::instance().get(npc_id));
    npc->reset(npc_id, 50.0f, 50.0f);
    scene_->AddNpc(npc);

    // 2. 初始 Tick，周围没有任何玩家
    scene_->OnTick();
    EXPECT_FALSE(npc->IsAiActive()) << "Error: NPC should be sleeping when no player is in AOI";

    // 3. 创建玩家，并放置在距离极远的 (10, 10) 处
    // 传入 nullptr 满足 PlayerActor 需要 std::shared_ptr<Connection> 的构造要求
    ActorID player_id = ActorRegistry::instance().create_actor<PlayerActor>(nullptr);

    // 模拟玩家进入场景 (严格按照业务带参构造)
    SceneEnterMsg enter_msg(player_id, player_id.raw, 10.0f, 10.0f);
    scene_->handle_message(&enter_msg);

    // 触发 Tick 处理场景消息和 AI 探查
    scene_->OnTick();
    EXPECT_FALSE(npc->IsAiActive()) << "Error: NPC should still be sleeping, player is too far (out of AOI)";

    // 4. 模拟玩家移动到 NPC 附近 (45, 45) -> 此时一定进入了 50,50 的九宫格视野
    // 参数: ActorID, uid, aoi_grid_index (这里测试给个0即可), newX, newY, direction
    SceneMoveMsg move_msg(player_id, player_id.raw, 0, 45.0f, 45.0f, 0);
    scene_->handle_message(&move_msg);

    PlayerActor *player = static_cast<PlayerActor *>(ActorRegistry::instance().get(player_id));
    player->MarkDirty(PlayerActor::DIRTY_POS);

    // 触发 Tick，SceneActor 处理 Move 并更新 AOI 后，探查 AI
    scene_->OnTick();
    EXPECT_TRUE(npc->IsAiActive()) << "Success: NPC woke up because player entered AOI!";

    // 清理
    ActorRegistry::instance().remove(npc_id);
    ActorRegistry::instance().remove(player_id);
}

// ===================================================================
// KR3 测试：行为树逻辑 (索敌与逼近)
// ===================================================================
TEST_F(SceneAiTest, NpcBehaviorMoveTowardsPlayer)
{
    // 1. 创建 NPC 在 (50, 50)
    ActorID npc_id = ActorRegistry::instance().create_actor<NpcActor>();
    NpcActor *npc = static_cast<NpcActor *>(ActorRegistry::instance().get(npc_id));
    npc->reset(npc_id, 50.0f, 50.0f);
    scene_->AddNpc(npc);

    // 2. 创建玩家，直接降落到视野内 (55, 50)，距离为 5
    ActorID player_id = ActorRegistry::instance().create_actor<PlayerActor>(nullptr);
    SceneEnterMsg enter_msg(player_id, player_id.raw, 55.0f, 50.0f);
    scene_->handle_message(&enter_msg);

    // 记录初始坐标
    float initial_npc_x = npc->GetX();

    // 3. 执行单帧 Tick
    // 预期行为：
    // - SceneActor 发现玩家在视野内，激活 NPC AI (is_ai_active_ = true)
    // - NPC 执行行为树：FindPlayer 成功 -> MoveTowards 开始执行 -> 向右平移
    scene_->OnTick();

    // 4. 验证 AI 是否被唤醒并发生了移动
    EXPECT_TRUE(npc->IsAiActive());

    // 因为玩家在右边(x=55)，NPC(x=50) 应该向右移动，X 坐标应该增加
    EXPECT_GT(npc->GetX(), initial_npc_x) << "Error: NPC did not move towards the player!";

    // 如果你在 MoveTowards 里设置了 speed = 0.5f，这里甚至可以精确断言
    // EXPECT_FLOAT_EQ(npc->GetX(), 50.5f);

    // 清理
    ActorRegistry::instance().remove(npc_id);
    ActorRegistry::instance().remove(player_id);
}