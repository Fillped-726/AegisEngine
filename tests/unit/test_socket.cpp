#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include "aegis/net/socket.h"
#include "aegis/core/env.h"
#include "aegis/core/task.h"

using namespace aegis;

class SocketTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        // Initialize the io_uring environment once for all tests
        core::Env::instance().init(1024);
    }

    // Helper to create a connected pair of sockets (Unix Domain Sockets)
    // This allows us to test send/recv without needing a full TCP handshake
    std::pair<net::Socket, net::Socket> create_socket_pair()
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        {
            throw std::runtime_error("socketpair failed");
        }
        // Set both to non-blocking (crucial for io_uring/async)
        for (int fd : sv)
        {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        return {net::Socket(sv[0]), net::Socket(sv[1])};
    }
};

// 1. Test Async Write and Read (Basic)
TEST_F(SocketTest, AsyncSendAndRecv)
{
    auto [s1, s2] = create_socket_pair();

    const std::string payload = "Hello Aegis";
    std::vector<char> buffer(1024);
    bool done = false;

    // Define the async task
    auto task = [&, s1 = std::move(s1), s2 = std::move(s2)]() mutable -> core::DetachedTask
    {
        // Step 1: Write to S1
        int sent = co_await s1.send(payload.data(), payload.size());
        EXPECT_EQ(sent, payload.size());

        // Step 2: Read from S2
        int received = co_await s2.recv(buffer.data(), buffer.size());
        EXPECT_EQ(received, payload.size());
        EXPECT_EQ(std::string(buffer.data(), received), payload);

        done = true;
    };

    task(); // Start coroutine

    // Drive the Event Loop until done
    // In a real app, Env::run() blocks, but for tests we might need to manually tick or run briefly
    // Here we assume Env runs in a separate thread or we just run it for a bit.

    // Simple way for unit test: run the loop in a thread for a short time
    std::thread runner([]
                       { core::Env::instance().run(); });

    // Wait for completion (with timeout)
    for (int i = 0; i < 100; ++i)
    {
        if (done)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // In a real robust test, you'd want a way to stop Env::run() cleanly.
    // For now, we detach or let it run (setup/teardown issues aside).
    runner.detach();

    EXPECT_TRUE(done);
}

// 2. Test Scatter/Gather I/O (ReadV/WriteV)
TEST_F(SocketTest, AsyncWriteVAndReadV)
{
    auto [s1, s2] = create_socket_pair();
    bool done = false;

    auto task = [&, s1 = std::move(s1), s2 = std::move(s2)]() mutable -> core::DetachedTask
    {
        // Data chunks
        std::string header = "HEAD";
        std::string body = "BODYDATA";

        // Prepare WriteV
        std::vector<struct iovec> w_iov(2);
        w_iov[0].iov_base = const_cast<char *>(header.data());
        w_iov[0].iov_len = header.size();
        w_iov[1].iov_base = const_cast<char *>(body.data());
        w_iov[1].iov_len = body.size();

        // Step 1: WriteV
        int sent = co_await s1.send(w_iov);
        EXPECT_EQ(sent, header.size() + body.size());

        // Prepare ReadV (Receive into separate buffers)
        char head_buf[4];
        char body_buf[8];
        std::vector<struct iovec> r_iov(2);
        r_iov[0].iov_base = head_buf;
        r_iov[0].iov_len = 4;
        r_iov[1].iov_base = body_buf;
        r_iov[1].iov_len = 8;

        // Step 2: ReadV
        int received = co_await s2.recv(r_iov);
        EXPECT_EQ(received, 12);

        EXPECT_EQ(std::string(head_buf, 4), "HEAD");
        EXPECT_EQ(std::string(body_buf, 8), "BODYDATA");

        done = true;
    };

    task();

    std::thread runner([]
                       { core::Env::instance().run(); });
    for (int i = 0; i < 100; ++i)
    {
        if (done)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runner.detach();

    EXPECT_TRUE(done);
}

// 3. Test Error Handling (Robustness)
TEST_F(SocketTest, HandlesClosedPeer)
{
    auto [s1, s2] = create_socket_pair();
    bool caught_exception = false;

    auto task = [&, s1 = std::move(s1), s2 = std::move(s2)]() mutable -> core::DetachedTask
    {
        // Close the reader end
        s2.close();

        // Write to closed socket -> Should trigger EPIPE (Broken pipe)
        // Note: In async land, the first write might succeed (buffered),
        // but eventually it should fail or return 0.
        // Or if we wait for a read on a closed socket:

        try
        {
            char buf[10];
            // Reading from closed s2
            // Wait, s2 is closed, let's try reading from s1 after closing s2
            // (Simulate peer reset)
            int res = co_await s1.recv(buf, 10);
            // On socket closure, recv usually returns 0 (EOF), not exception.
            if (res == 0)
                caught_exception = true;
        }
        catch (const std::system_error &e)
        {
            // Some specific errors might throw
        }
    };

    task();
    // (Run loop logic same as above...)
    std::thread runner([]
                       { core::Env::instance().run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    runner.detach();

    // We expect graceful EOF handling (res == 0) or exception depending on your logic
    // Your code throws if result < 0. EOF is result == 0.
    EXPECT_TRUE(caught_exception);
}