#include <iostream>
#include <vector>
#include <memory>
#include <cassert>
#include <cstring>
#include <source_location>

// 引入核心业务头文件
// 注意：现在使用的是 SceneActor
#include "aegis/core/scene_actor.h"
#include "aegis/core/playerActor.h"
#include "aegis/core/message.h" // for ActorMessage
#include "scene.pb.h"           // 引入 Proto

// ========================================================
// 1. Mock Infrastructure (测试基础设施)
// ========================================================

// Mock Connection: 拦截发包，用于验证
class MockConnection : public aegis::net::Connection
{
public:
    // 构造一个无效的 FD (-1)
    MockConnection() : aegis::net::Connection(aegis::net::Socket()) {}

    // Mock send 方法
    // 实际项目中 Connection::send 最好是 virtual 的，或者 PlayerActor 依赖 IConnection 接口
    // 这里假设我们已经把 Connection::send 改为了 virtual，或者我们 hack 了 PlayerActor::send_packet
    // 简单起见，我们假设通过继承覆盖了 send 逻辑 (需要 Connection 类支持)
    void send(aegis::net::Packet pkt) override
    {
        // 捕获发出的包
        captured_packets.push_back(std::move(pkt));
    }

    void clear() { captured_packets.clear(); }

    std::vector<aegis::net::Packet> captured_packets;
};

// 测试断言宏
void LogFail(const char *expr, std::source_location loc = std::source_location::current())
{
    std::cerr << "[FAIL] " << expr << " at " << loc.file_name() << ":" << loc.line() << std::endl;
    std::exit(1);
}
#define EXPECT_TRUE(cond) \
    if (!(cond))          \
        LogFail(#cond);
#define EXPECT_EQ(a, b) \
    if ((a) != (b))     \
        LogFail(#a " == " #b);

// ========================================================
// 2. Test Logic (核心测试逻辑)
// ========================================================

void Test_Move_EnterView()
{
    std::cout << "[Test] Starting SceneActor Logic Verification..." << std::endl;

    // 1. 初始化 SceneActor (替代原来的 Scene)
    // 100x100, 格子大小 10
    auto sceneActor = std::make_shared<aegis::core::SceneActor>(100.0f, 100.0f, 10.0f);

    // 2. 创建 Mock 连接
    auto connA = std::make_shared<MockConnection>();
    auto connB = std::make_shared<MockConnection>();

    // 3. 创建 PlayerActor (必须用 shared_ptr，因为 SceneActor 会持有)
    // A 在 (0,0), ID 10
    auto actorA = new aegis::core::PlayerActor(connA, 10);
    actorA->SetPos(0.0f, 0.0f);

    // B 在 (50,50), ID 20
    auto actorB = new aegis::core::PlayerActor(connB, 20);
    actorB->SetPos(50.0f, 50.0f);

    // 4. 发送 [Enter Msg] 模拟玩家进入场景
    // 在真实服务器中，这是由 Scheduler 调度的。
    // 在单元测试中，我们直接调用 handle_message 来模拟 Worker 线程执行。

    // Player A 进入
    {
        aegis::core::SceneEnterMsg msg(actorA, 0.0f, 0.0f);
        sceneActor->handle_message(&msg);
    }

    // Player B 进入
    {
        aegis::core::SceneEnterMsg msg(actorB, 50.0f, 50.0f);
        sceneActor->handle_message(&msg);
    }

    std::cout << ">> Setup: Players entered scene." << std::endl;

    // 清空之前的包 (比如 Enter 产生的包，我们这次只测 Move 触发的 EnterView)
    connA->clear();
    connB->clear();

    // 5. 执行核心操作：A 移动到 B 附近 (50, 45)
    // 触发 AOI 变化：A 应该看见 B，B 应该看见 A

    std::cout << ">> Action: Player A moves to (50, 45)" << std::endl;

    // 构造 Move 消息
    // oldX=0,0, newX=50,45
    // 注意：在发消息前，PlayerActor 的坐标可能还没变，也可能变了，取决于架构。
    // 这里我们显式传入 old 和 new 给 SceneActor
    aegis::core::SceneMoveMsg moveMsg(10, 0.0f, 0.0f, 50.0f, 45.0f);

    // 模拟 Scheduler 调度 SceneActor 处理该消息
    sceneActor->handle_message(&moveMsg);

    // 更新 A 的本地坐标 (模拟 PlayerActor 处理完业务后的状态同步)
    actorA->SetPos(50.0f, 45.0f);

    // ========================================================
    // 6. 验证结果 (Verification)
    // ========================================================

    // --- 验证 A (Mover) 收到的包 ---
    // 预期：A 应该收到 SCEnterViewNtf (1003)，里面包含 B (ID 20)
    {
        std::cout << ">> Verifying A received B's info..." << std::endl;

        bool foundB = false;
        // 遍历 A 收到的所有包
        for (const auto &pkt : connA->captured_packets)
        {
            // 解析包头获取 MsgID (前 4 字节是长度，不属于 Packet body 的一部分，Packet 类应该处理好了)
            // 假设 Packet::msg_id() 能正确返回 ID (MockConnection 只是存了 Packet 对象)
            // 我们的 Packet 结构是: Header(4 byte len + 4 byte id) + Body
            // 这里的 pkt.msg_id() 是我们为了测试方便假设存在的 helper，
            // 或者我们需要手动解析:
            uint32_t msg_id = 0;
            if (pkt.payload_.size() >= 4)
            {
                // 网络字节序转主机字节序
                uint32_t net_id;
                std::memcpy(&net_id, pkt.payload_.data(), 4);
                msg_id = ntohl(net_id);
            }

            if (msg_id == 1003) // SC_ENTER_VIEW_NTF
            {
                aegis::protocol::SCEnterViewNtf ntf;
                // 跳过 4 字节 MsgID 解析 Body
                if (ntf.ParseFromArray(pkt.payload_.data() + 4, pkt.payload_.size() - 4))
                {
                    for (const auto &entity : ntf.entities())
                    {
                        if (entity.entity_id() == 20)
                        {
                            foundB = true;
                            std::cout << "   [PASS] A saw Entity 20 (Name: " << entity.name() << ")" << std::endl;
                        }
                    }
                }
            }
        }
        EXPECT_TRUE(foundB);
    }

    // --- 验证 B (Observer) 收到的包 ---
    // 预期：B 应该收到 SCEnterViewNtf (1003)，里面包含 A (ID 10)
    {
        std::cout << ">> Verifying B received A's info..." << std::endl;

        bool foundA = false;
        for (const auto &pkt : connB->captured_packets)
        {
            uint32_t msg_id = 0;
            if (pkt.payload_.size() >= 4)
            {
                uint32_t net_id;
                std::memcpy(&net_id, pkt.payload_.data(), 4);
                msg_id = ntohl(net_id);
            }

            if (msg_id == 1003)
            {
                aegis::protocol::SCEnterViewNtf ntf;
                if (ntf.ParseFromArray(pkt.payload_.data() + 4, pkt.payload_.size() - 4))
                {
                    for (const auto &entity : ntf.entities())
                    {
                        if (entity.entity_id() == 10)
                        {
                            foundA = true;
                            std::cout << "   [PASS] B saw Entity 10 (Name: " << entity.name() << ")" << std::endl;
                        }
                    }
                }
            }
        }
        EXPECT_TRUE(foundA);
    }

    std::cout << "[SUCCESS] Test_Move_EnterView Passed." << std::endl;
}

int main()
{
    Test_Move_EnterView();
    return 0;
}