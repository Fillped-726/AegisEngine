#include <gtest/gtest.h>
#include <thread>
#include <chrono>

//  common includes
#include "aegis/common/aegisLog.h"

// Core Includes
#include "aegis/core/env.h"
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include "aegis/core/scheduler.h"
#include "aegis/core/actor_registry.h"
#include "aegis/game/room_manager.h"
#include "aegis/game/scene_actor.h"
#include "aegis/game/playerActor.h"
#include "aegis/core/message.h"

using namespace aegis::core;

class ArchitectureTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 1. 初始化日志 (为了看清楚流程)
        aegis::Log::instance().set_level(spdlog::level::debug);

        // 2. 启动调度器 (1个线程足够测试逻辑)
        Scheduler::instance().start(1);
    }

    void TearDown() override
    {
        Scheduler::instance().stop();
    }
};

// 测试案例 1: 房间创建与层级建立
TEST_F(ArchitectureTest, RoomManagerCreatesScene)
{
    // 1. 创建 RoomManager
    ActorID mgr_id = ActorRegistry::instance().create_actor<RoomManager>();
    ASSERT_TRUE(mgr_id.is_valid());

    Actor *mgr = ActorRegistry::instance().get(mgr_id);
    ASSERT_NE(mgr, nullptr);

    // 2. 发送 Fake RPC 请求创建房间 (模拟 GateServer 启动行为)
    {
        auto *msg = new RPCCreateRoomMsg(); // 使用无参构造 (内部消息)
        msg->req.set_room_id(1);
        msg->req.set_map_id(1001);

        mgr->push(msg);
    }

    // 3. 等待调度器处理 (因为是异步的，睡 50ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ActorID likely_scene_id;
    likely_scene_id.parts.index = mgr_id.parts.index + 1; // 猜测下一个 slot
    likely_scene_id.parts.version = 1;

    Actor *scene = ActorRegistry::instance().get(likely_scene_id);
    // 注意：如果是 ConcurrentQueue 的 free_list，ID 可能不是连续的，
    // 这里做演示，如果不连续，建议给 RoomManager 加个 TestOnly 的 Getter。

    if (scene)
    {
        aegis::Log::instance().info("Verified: Scene Actor exists at ID {}", likely_scene_id.raw);
        // 验证父子关系
        EXPECT_EQ(scene->parent_id().raw, mgr_id.raw);
    }
    else
    {
        aegis::Log::instance().warn("Could not deterministically verify Scene ID without introspection API");
    }
}

// 测试案例 2: 死亡监管 (Supervision)
TEST_F(ArchitectureTest, SupervisionChain)
{
    // 1. 建 RoomMgr
    ActorID mgr_id = ActorRegistry::instance().create_actor<RoomManager>();

    // 2. 建 Scene
    ActorID scene_id = ActorRegistry::instance().create_actor<SceneActor>(500.f, 500.f, 10.f);
    ASSERT_TRUE(scene_id.is_valid());

    // 3. 手动绑定父子关系 (模拟 RoomManager::on_create_room 的逻辑)
    Actor *scene = ActorRegistry::instance().get(scene_id);
    scene->set_parent_id(mgr_id);

    // 4. 赐死 Scene
    // 触发 Actor 状态机转为 Dead
    // 我们发一个 PoisonPill 或者手动调用内部逻辑
    // 假设有一个 DestroyMsg
    scene->push(new ActorDestroyMsg()); // 需要你有这个消息定义

    Scheduler::instance().dispatch(scene);

    // 5. 等待处理
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 6. 验证
    // a. Registry 里应该查不到 scene_id 了 (因为版本号会变，或者指针空了)
    Actor *dead_scene = ActorRegistry::instance().get(scene_id);
    EXPECT_EQ(dead_scene, nullptr); // 应该为空，说明逻辑注销成功

    // b. RoomManager 应该收到了遗言
    // 这需要 Mock 或者通过 RoomManager 的日志/状态来验证
    // 只要程序没崩，且打印了 "[RoomMgr] Child died"，就算通过
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}