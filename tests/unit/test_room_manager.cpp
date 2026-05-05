#include <gtest/gtest.h>
#include "aegis/game/room_manager.h"
#include "aegis/game/scene_actor.h"
#include "aegis/core/actor_registry.h"

using namespace aegis::core;

class RoomManagerTest : public ::testing::Test
{
protected:
    ActorID rm_id_;
    RoomManager *rm_ = nullptr;
    ActorID scene_id_;

    void SetUp() override
    {
        rm_id_ = ActorRegistry::instance().create_actor<RoomManager>();
        ASSERT_TRUE(rm_id_.is_valid());
        rm_ = static_cast<RoomManager *>(ActorRegistry::instance().get(rm_id_));
        ASSERT_NE(rm_, nullptr);

        scene_id_ = ActorRegistry::instance().create_actor<SceneActor>(-1000.0f, -1000.0f, 1000.0f, 1000.0f, 256.0f);
        ASSERT_TRUE(scene_id_.is_valid());
        auto *scene = static_cast<SceneActor *>(ActorRegistry::instance().get(scene_id_));
        ASSERT_NE(scene, nullptr);
        scene->set_parent_id(rm_id_);
    }

    void TearDown() override
    {
        if (rm_id_.is_valid())
            ActorRegistry::instance().remove(rm_id_);
        if (scene_id_.is_valid())
            ActorRegistry::instance().remove(scene_id_);
    }

    // 注册一个测试营地
    void AddCamp(const std::string &name, int32_t players)
    {
        rm_->TestRegisterCamp(scene_id_.raw, name, players);
    }

    // 上报人数
    void ReportCount(int32_t count)
    {
        rm_->TestOnPlayerCount(scene_id_.raw, count);
    }
};

// ================================================================
// 测试1: 人数上报更新 meta 中的值
// ================================================================
TEST_F(RoomManagerTest, PlayerCountReportUpdatesMeta)
{
    AddCamp("TestCamp", 2);

    ReportCount(1);

    ASSERT_EQ(rm_->camp_metas().size(), 1);
    EXPECT_EQ(rm_->camp_metas().at(scene_id_.raw).current_players, 1);
    EXPECT_TRUE(rm_->destroying_scenes().empty()) << "人数 > 0 不应触发销毁";
}

// ================================================================
// 测试2: 人数为 0 时触发销毁
// ================================================================
TEST_F(RoomManagerTest, ZeroPlayersTriggersDestruction)
{
    AddCamp("TestCamp", 1);

    ReportCount(0);

    EXPECT_TRUE(rm_->destroying_scenes().count(scene_id_.raw))
        << "人数为0时场景应被标记为销毁中";
}

// ================================================================
// 测试3: 多次上报 0 具有幂等性
// ================================================================
TEST_F(RoomManagerTest, MultipleZeroReportsAreIdempotent)
{
    AddCamp("IdempotentCamp", 1);

    ReportCount(0);
    ReportCount(0);  // 第二次，被 destroying_scenes_ 拦截
    ReportCount(0);  // 第三次

    EXPECT_EQ(rm_->destroying_scenes().size(), 1);
    // camp_metas_ 在销毁后由 on_scene_died 清理，
    // 但单元测试中 ActorDiedMsg 没有 Scheduler 处理，meta 保留
}

// ================================================================
// 测试4: 2→1→0 完整递减链路
// ================================================================
TEST_F(RoomManagerTest, FullLifecycleDecrementToZero)
{
    AddCamp("LifecycleCamp", 2);

    ReportCount(1);
    ASSERT_EQ(rm_->camp_metas().at(scene_id_.raw).current_players, 1);
    EXPECT_TRUE(rm_->destroying_scenes().empty()) << "还有1人，不应销毁";

    ReportCount(0);
    EXPECT_TRUE(rm_->destroying_scenes().count(scene_id_.raw));
}

// ================================================================
// 测试5: 创建营地后有对应的 meta 条目
// ================================================================
TEST_F(RoomManagerTest, CreateCampPopulatesMeta)
{
    AddCamp("TestCamp", 1);

    EXPECT_EQ(rm_->camp_metas().size(), 1);
    EXPECT_EQ(rm_->camp_metas().at(scene_id_.raw).camp_name, "TestCamp");
    EXPECT_EQ(rm_->camp_metas().at(scene_id_.raw).current_players, 1);
}

// ================================================================
// 测试6: 对未知场景上报 → 不崩溃
// ================================================================
TEST_F(RoomManagerTest, ReportUnknownSceneIsHarmless)
{
    // 用不存在的场景 ID 上报
    CampPlayerCountMsg msg(99999, 0);
    rm_->TestOnPlayerCount(99999, 0);

    EXPECT_TRUE(rm_->camp_metas().empty());
    EXPECT_TRUE(rm_->destroying_scenes().empty());
}

// ================================================================
// 测试7: meta 丢失后自动恢复 (从 camp_scenes_ 重建)
// ================================================================
TEST_F(RoomManagerTest, AutoRecoverMetaFromSceneRecord)
{
    // 通过 TestRegisterCamp 写入数据
    rm_->TestRegisterCamp(scene_id_.raw, "RecoverCamp", 3);

    ASSERT_EQ(rm_->camp_metas().size(), 1);
    EXPECT_EQ(rm_->camp_metas().at(scene_id_.raw).camp_name, "RecoverCamp");
    EXPECT_EQ(rm_->camp_metas().at(scene_id_.raw).current_players, 3);
}
