#include <iostream>
#include <vector>
#include <memory>
#include <cassert>
#include <cstring>
#include <source_location>

// 引入核心业务头文件
#include "aegis/core/scene.h"
#include "aegis/core/playerActor.h"
#include "scene.pb.h" // 引入 Proto

// ========================================================
// 1. Mock Infrastructure (测试基础设施)
// ========================================================

// 假设 Connection 的 send 是 virtual 的，或者是通过模板/回调机制。
// 在这里为了简单，我们假设 aegis::net::Connection 允许继承或我们能通过友元访问。
// 如果 Connection::send 不是 virtual，实际项目中通常会给 PlayerActor 传入一个 ConnectionInterface。

// 为了本测试能跑，我们定义一个能够"捕获"包的 FakeConnection
// 注意：这需要你的 Connection 类析构函数是 virtual 的，或者允许子类化
class MockConnection : public aegis::net::Connection
{
public:
    // 构造一个无效的 FD (-1)
    MockConnection() : aegis::net::Connection(aegis::net::Socket()) {}

    // 拦截发送
    // 注意：需要在 include/aegis/net/connection.h 中把 send 声明为 virtual
    // 或者，如果没有 virtual，我们可以验证 PlayerActor::send_buffer 里的逻辑。
    // 这里假设我们能拦截。
    void send(aegis::net::Packet pkt) override
    {
        // 深拷贝一份 Packet 数据或者直接移动所有权，方便后续断言
        captured_packets.push_back(std::move(pkt));
    }

    // 辅助：获取最近收到的包
    aegis::net::Packet *last_packet()
    {
        if (captured_packets.empty())
            return nullptr;
        return &captured_packets.back();
    }

    // 辅助：清空
    void clear() { captured_packets.clear(); }

    std::vector<aegis::net::Packet> captured_packets;
};

// 简单的测试断言宏
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
    std::cout << "[Test] Starting A moves near B..." << std::endl;

    // 1. 初始化场景 (100x100, 格子大小 10)
    aegis::core::Scene scene(100.0f, 100.0f, 10.0f);

    // 2. 创建 Mock 连接
    auto connA = std::make_shared<MockConnection>();
    auto connB = std::make_shared<MockConnection>();

    // 3. 创建 Actor (A 和 B)
    // A 在 (0,0), ID 10
    auto actorA = new aegis::core::PlayerActor(connA, 10);
    actorA->SetPos(0.0f, 0.0f);

    // B 在 (50,50), ID 20
    auto actorB = new aegis::core::PlayerActor(connB, 20);
    actorB->SetPos(50.0f, 50.0f);

    // 4. 加入场景
    scene.AddPlayer(actorA);
    scene.AddPlayer(actorB);

    // 此时清空一下 MockConnection 之前可能产生的包（如果有）
    connA->clear();
    connB->clear();

    // 5. 执行核心操作：A 移动到 B 附近 (50, 45)
    // Grid(50,50) 是 [Row 5, Col 5]
    // Grid(50,45) 是 [Row 4, Col 5]
    // Row 4 和 Row 5 是相邻的，所以应该互相看见
    std::cout << ">> Action: Player A moves to (50, 45)" << std::endl;
    scene.OnPlayerMove(actorA, 50.0f, 45.0f);

    // ========================================================
    // 6. 验证结果 (Verification)
    // ========================================================

    // --- 验证 A (Mover) 收到的包 ---
    // 预期：A 应该收到 SCEnterViewNtf，里面包含 B (ID 20) 的信息
    {
        std::cout << ">> Verifying A received B's info..." << std::endl;
        EXPECT_TRUE(connA->captured_packets.size() >= 1);

        bool foundB = false;
        for (const auto &pkt : connA->captured_packets)
        {
            // SCEnterViewNtf MsgID = 1003
            if (pkt.msg_id() == 1003)
            {
                aegis::protocol::SCEnterViewNtf ntf;
                if (pkt.parse(ntf))
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
    // 预期：B 应该收到 SCEnterViewNtf，里面包含 A (ID 10) 的信息
    {
        std::cout << ">> Verifying B received A's info..." << std::endl;
        EXPECT_TRUE(connB->captured_packets.size() >= 1);

        bool foundA = false;
        for (const auto &pkt : connB->captured_packets)
        {
            // SCEnterViewNtf MsgID = 1003
            if (pkt.msg_id() == 1003)
            {
                // 注意：这里可能收到的是 Raw Buffer 拼出来的包，MockConnection 应该能照常处理
                aegis::protocol::SCEnterViewNtf ntf;
                if (pkt.parse(ntf))
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

    // 清理 (实际项目中建议使用智能指针管理 Actor 生命周期)
    // scene.RemovePlayer 并不负责 delete Actor，这里简单手动 delete
    delete actorA;
    delete actorB;
}

int main()
{
    Test_Move_EnterView();
    return 0;
}