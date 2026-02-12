#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <future> // [修复] 必须包含这个
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#include "aegis/core/env.h"
#include "aegis/net/connection.h"
#include "aegis/net/packet.h"

using namespace aegis;

class ConnectionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 1. 初始化 Env
        core::Env::instance().init(2048);

        // 2. 启动后台线程运行 Env Loop
        env_thread_ = std::thread([]
                                  { core::Env::instance().run(); });
    }

    void TearDown() override
    {
        // [修复] 现在 Env 有了 stop 方法
        core::Env::instance().stop();
        if (env_thread_.joinable())
        {
            env_thread_.join();
        }
    }

    // 创建阻塞模式的 SocketPair
    std::pair<net::Socket, int> create_blocking_pair()
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        {
            throw std::runtime_error("socketpair failed");
        }
        return {net::Socket(sv[0]), sv[1]};
    }

    // 辅助：生成 Packet
    net::PooledPacket make_packet(const std::string &msg)
    {
        auto pkt = std::make_unique<net::Packet>();
        pkt->alloc(msg.size());
        std::memcpy(pkt->mutable_data(), msg.data(), msg.size());
        return pkt;
    }

    std::thread env_thread_;
};

// 测试用例 1: 异步发送集成测试
TEST_F(ConnectionTest, AsyncSendIntegration)
{
    auto [sock_conn, fd_tester] = create_blocking_pair();
    auto conn = std::make_shared<net::Connection>(std::move(sock_conn));

    std::string msg = "Hello Aegis";
    conn->send(make_packet(msg));

    // 验证逻辑
    uint32_t net_len;
    // [修复] 使用 ASSERT_EQ 检查 write/read 返回值，消除警告
    ssize_t n = ::read(fd_tester, &net_len, 4);
    ASSERT_EQ(n, 4);

    uint32_t body_len = ntohl(net_len);
    ASSERT_EQ(body_len, msg.size());

    std::vector<char> buf(body_len);
    n = ::read(fd_tester, buf.data(), body_len);
    ASSERT_EQ(n, body_len);
    ASSERT_EQ(std::string(buf.data(), n), msg);
}

// 测试用例 2: 粘包读取测试
TEST_F(ConnectionTest, ReadCoalescedPackets)
{
    auto [sock_conn, fd_tester] = create_blocking_pair();
    auto conn = std::make_shared<net::Connection>(std::move(sock_conn));

    std::string msg1 = "Short";
    std::string msg2 = "A slightly longer message";

    std::vector<char> buffer;
    auto append = [&](const std::string &m)
    {
        uint32_t l = htonl(m.size());
        const char *p = (const char *)&l;
        buffer.insert(buffer.end(), p, p + 4);
        buffer.insert(buffer.end(), m.begin(), m.end());
    };
    append(msg1);
    append(msg2);

    // [修复] 处理 write 返回值警告
    ssize_t written = ::write(fd_tester, buffer.data(), buffer.size());
    ASSERT_EQ(written, buffer.size());

    std::promise<void> p1, p2;
    auto f1 = p1.get_future();
    auto f2 = p2.get_future();

    // 启动读取任务
    auto task = [conn, &p1, &p2, msg1, msg2]() -> core::DetachedTask
    {
        try
        {
            auto pkt1 = co_await conn->read_packet();
            if (pkt1 && std::string(pkt1->data(), pkt1->size()) == msg1)
                p1.set_value();
            else
                p1.set_exception(std::make_exception_ptr(std::runtime_error("Pkt1 mismatch")));

            auto pkt2 = co_await conn->read_packet();
            if (pkt2 && std::string(pkt2->data(), pkt2->size()) == msg2)
                p2.set_value();
            else
                p2.set_exception(std::make_exception_ptr(std::runtime_error("Pkt2 mismatch")));
        }
        catch (...)
        {
            try
            {
                p1.set_exception(std::current_exception());
            }
            catch (...)
            {
            }
        }
    };
    task();

    // [修复] future_status 在 C++ 标准中确实存在，但有时受实现影响
    // 更稳妥的写法是直接 wait_for 检查状态
    ASSERT_EQ(f1.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    ASSERT_EQ(f2.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    // 确保没有异常抛出
    f1.get();
    f2.get();
}

// 测试用例 3: 碎片包读取测试
TEST_F(ConnectionTest, ReadFragmentedPacket)
{
    auto [sock_conn, fd_tester] = create_blocking_pair();
    auto conn = std::make_shared<net::Connection>(std::move(sock_conn));

    std::string msg = "Fragmentation Test";
    uint32_t len = htonl(msg.size());

    std::promise<void> done;
    auto fut = done.get_future();

    auto task = [conn, &done, msg]() -> core::DetachedTask
    {
        try
        {
            auto pkt = co_await conn->read_packet();
            if (pkt && std::string(pkt->data(), pkt->size()) == msg)
                done.set_value();
            else
                done.set_exception(std::make_exception_ptr(std::runtime_error("Content mismatch")));
        }
        catch (...)
        {
            done.set_exception(std::current_exception());
        }
    };
    task();

    // 1. 发送 Header
    ssize_t n = ::write(fd_tester, &len, 4);
    ASSERT_EQ(n, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 2. 发送 Body 前半段
    n = ::write(fd_tester, msg.data(), 5);
    ASSERT_EQ(n, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 3. 发送 Body 后半段
    n = ::write(fd_tester, msg.data() + 5, msg.size() - 5);
    ASSERT_EQ(n, msg.size() - 5);

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    fut.get();
}