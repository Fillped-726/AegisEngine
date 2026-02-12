#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <arpa/inet.h>
#include "aegis/net/acceptor.h"
#include "aegis/core/env.h"
#include "aegis/core/task.h"

using namespace aegis;

// Test Fixture: 负责初始化和清理 io_uring 环境
class AcceptorTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        // 初始化 Env，只需一次
        // 使用较小的队列深度即可
        core::Env::instance().init(1024);
    }

    static void TearDownTestSuite()
    {
        // 如果 Env 有析构清理逻辑，这里什么都不用做
        // 或者手动调用 stop (取决于你的 Env 实现)
    }
};

// 辅助函数：用原生 socket 发起一个阻塞连接，用于触发 accept
void simple_blocking_connect(int port)
{
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // 尝试连接，带重试
    for (int i = 0; i < 5; ++i)
    {
        if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(sock);
}

TEST_F(AcceptorTest, BindToEphemeralPort)
{
    // Port 0 让操作系统自动选择一个可用端口
    net::Acceptor acceptor(0);

    // 验证 FD 是否有效
    EXPECT_GT(acceptor.native_handle(), 0);

    // 验证端口是否已分配 (应该 > 0)
    EXPECT_GT(acceptor.port(), 0);

    std::cout << "[Info] Bound to random port: " << acceptor.port() << std::endl;
}

TEST_F(AcceptorTest, DetectsPortConflict)
{
    int port = 9999;

    // 1. 创建第一个 Acceptor，占用端口
    net::Acceptor a1(port);
    EXPECT_GT(a1.native_handle(), 0);

    // 2. 尝试创建第二个 Acceptor 绑定同一端口
    // 预期：应该抛出 std::runtime_error (根据你之前的实现)
    EXPECT_THROW({ net::Acceptor a2(port); }, std::runtime_error);
}

TEST_F(AcceptorTest, ReuseAddress)
{
    int port = 9998;

    {
        net::Acceptor a1(port);
        // a1 在这里析构，socket 关闭
        // 但在 TCP 协议中，端口可能进入 TIME_WAIT 状态
    }

    // 立即再次绑定同一端口
    // 如果 SO_REUSEADDR 设置正确，这里应该成功，不会抛异常
    EXPECT_NO_THROW({
        net::Acceptor a2(port);
    });
}

TEST_F(AcceptorTest, InvalidIPAddress)
{
    // 这是一个无效的 IP
    EXPECT_THROW({ net::Acceptor a(0, "999.999.999.999"); }, std::runtime_error);
}

TEST_F(AcceptorTest, AcceptConnection)
{
    // 1. 绑定随机端口
    auto acceptor = std::make_shared<net::Acceptor>(0);
    int port = acceptor->port();

    std::atomic<bool> accepted{false};

    // 2. 定义一个协程任务来执行 accept
    auto accept_task = [acceptor, &accepted]() -> core::DetachedTask
    {
        // 等待连接
        auto sock = co_await acceptor->accept();

        // 验证拿到的 socket 是否有效
        if (sock.is_valid())
        {
            accepted = true;
        }
        // 停止 Env 循环 (在生产代码中不要随便调 exit，这里是为了跳出 run 循环)
        // 注意：这里需要一种机制让 Env::run() 退出，或者我们只跑 1 秒
    };

    // 3. 启动服务端协程
    accept_task();

    // 4. 启动一个线程作为客户端发起连接
    std::thread client_thread([port]
                              {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        simple_blocking_connect(port); });

    // 5. 运行 IO 循环 (限制时间，防止死锁)
    // 这里我们用一种 hack 方式：让 Env 跑起来，但我们需要它能停下来
    // 如果你的 Env 没有 stop()，我们可以让它跑在一个线程里，然后 detach

    std::thread io_thread([]
                          { core::Env::instance().run(); });

    // 等待 accept 完成
    for (int i = 0; i < 10; ++i)
    {
        if (accepted)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 强制退出测试 (实际项目中 Env 应该支持 stop)
    // 这里为了演示简单，直接检查 atomic
    client_thread.join();
    io_thread.detach(); // 让它自生自灭，或者你的 Env 有 stop 方法最好

    EXPECT_TRUE(accepted);
}