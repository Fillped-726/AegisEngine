#include "aegis/core/env.h"
#include "aegis/net/socket.h"
#include "aegis/net/connection.h"
#include "aegis/net/packet.h"
#include "aegis/common/aegisLog.h"
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <cstring>
#include <liburing.h> // for timeout yield

using namespace aegis;

// --- 压测配置 ---
const int CLIENT_COUNT = 5;  // 并发连接数
const int PACKET_SIZE = 64;  // 包大小
const int BATCH_SEND = 10;   // 批处理大小
const int SAMPLE_RATE = 100; // 采样率：每 100 个包测一次延迟
const std::string SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 8888;

// --- 统计数据结构 ---
struct LatencyStats
{
    std::mutex lock;
    std::vector<long long> samples_us; // 微秒级延迟样本

    void add(long long us)
    {
        // 为了性能，这里不加锁，但在多线程下是不安全的
        // 正确做法是每个 Client 线程有自己的 vector，Monitor 线程去合并
        // 这里我们为了演示简化，直接加个轻量级锁，或者为了极限性能，
        // 我们只让每个线程维护自己的，Monitor 去读
    }
};

// 更好的设计：每个线程局部存储，Monitor 只读
// 但 vector 不是线程安全的。我们用一个简单的自旋锁保护写入。
struct ThreadSafeStats
{
    aegis::common::SpinLock lock;
    std::vector<long long> samples;

    void record(long long us)
    {
        std::lock_guard<aegis::common::SpinLock> lk(lock);
        samples.push_back(us);
    }

    // 取出并清空当前所有样本
    std::vector<long long> pop_all()
    {
        std::lock_guard<aegis::common::SpinLock> lk(lock);
        std::vector<long long> ret;
        ret.swap(samples);
        return ret;
    }
};

// 全局统计桶 (简化：只有一个全局桶，为了性能实际上应该 per-thread)
ThreadSafeStats g_latency_stats;

std::atomic<long long> g_sent_count{0};
std::atomic<long long> g_recv_count{0};
std::atomic<long long> g_recv_bytes{0};
std::atomic<bool> g_running{true};

// --- Yield Awaiter (必不可少) ---
struct AsyncYield : public aegis::core::BaseAwaiter
{
    struct __kernel_timespec ts{0, 0};
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        auto *ring = aegis::core::Env::instance().native_handle();
        auto *sqe = aegis::net::Socket::get_sqe_safe(ring);
        io_uring_prep_timeout(sqe, &ts, 0, 0);
        io_uring_sqe_set_data(sqe, static_cast<aegis::core::BaseAwaiter *>(this));
    }
    void await_resume() {}
};

// --- 包结构 ---
// 我们把时间戳藏在包的前8个字节里
struct BenchHeader
{
    int64_t send_time_ns;
    bool is_sample;
};

std::unique_ptr<net::Packet> make_bench_packet(bool sample)
{
    auto pkt = std::make_unique<net::Packet>();
    pkt->alloc(PACKET_SIZE);

    BenchHeader *header = reinterpret_cast<BenchHeader *>(pkt->mutable_data());
    header->is_sample = sample;

    if (sample)
    {
        auto now = std::chrono::steady_clock::now();
        header->send_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    }
    else
    {
        header->send_time_ns = 0;
    }

    return pkt;
}

// --- 客户端会话 ---
core::DetachedTask client_session(int id)
{
    // 1. 建立连接
    int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP.c_str(), &addr.sin_addr);

    if (::connect(raw_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(raw_fd);
        co_return;
    }

    int flags = fcntl(raw_fd, F_GETFL, 0);
    fcntl(raw_fd, F_SETFL, flags | O_NONBLOCK);

    net::Socket s(raw_fd);
    auto conn = std::make_shared<net::Connection>(std::move(s));

    // 2. Reader
    auto reader = [conn]() -> core::DetachedTask
    {
        try
        {
            while (g_running)
            {
                auto pkt = co_await conn->read_packet();
                if (!pkt)
                    break;

                // 解析时间戳
                if (pkt->size() >= sizeof(BenchHeader))
                {
                    auto *header = reinterpret_cast<const BenchHeader *>(pkt->data());
                    if (header->is_sample)
                    {
                        auto now = std::chrono::steady_clock::now();
                        int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                        long long latency_us = (now_ns - header->send_time_ns) / 1000;

                        // 记录延迟 (可能稍微有点锁竞争，但为了采样可以接受)
                        g_latency_stats.record(latency_us);
                    }
                }

                g_recv_count.fetch_add(1, std::memory_order_relaxed);
                g_recv_bytes.fetch_add(pkt->size(), std::memory_order_relaxed);
            }
        }
        catch (...)
        {
        }
    };
    reader();

    // 3. Sender
    int local_counter = 0;
    try
    {
        while (g_running)
        {
            for (int i = 0; i < BATCH_SEND; ++i)
            {
                local_counter++;
                // 每 SAMPLE_RATE 个包采样一次
                bool is_sample = (local_counter % SAMPLE_RATE == 0);
                conn->send(make_bench_packet(is_sample));
            }
            conn->flush();
            g_sent_count.fetch_add(BATCH_SEND, std::memory_order_relaxed);

            // 必须 Yield，否则 io_uring 没机会收包
            co_await AsyncYield{};
        }
    }
    catch (...)
    {
    }
}

// --- 监控与计算 ---
void monitor_thread()
{
    auto last_time = std::chrono::steady_clock::now();
    long long last_recv = 0;

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count() / 1000.0;

        long long current_recv = g_recv_count.load(std::memory_order_relaxed);
        long long qps = (long long)((current_recv - last_recv) / elapsed);

        // --- 计算 P99 ---
        std::vector<long long> samples = g_latency_stats.pop_all();
        long long p50 = 0, p99 = 0, p999 = 0, max_lat = 0;

        if (!samples.empty())
        {
            std::sort(samples.begin(), samples.end());
            p50 = samples[samples.size() * 0.50];
            p99 = samples[samples.size() * 0.99];
            p999 = samples[samples.size() * 0.999];
            max_lat = samples.back();
        }

        printf("[Bench] QPS: %-8lld | P50: %-4lld us | P99: %-4lld us | P99.9: %-4lld us | Max: %-4lld us | Samples: %zu\n",
               qps, p50, p99, p999, max_lat, samples.size());

        last_time = now;
        last_recv = current_recv;
    }
}

int main()
{
    Log::instance().set_level(spdlog::level::err);
    core::Env::instance().init(8192);

    std::cout << "Starting " << CLIENT_COUNT << " connections with latency sampling (1/" << SAMPLE_RATE << ")..." << std::endl;

    std::thread monitor(monitor_thread);
    monitor.detach();

    for (int i = 0; i < CLIENT_COUNT; ++i)
    {
        client_session(i);
        if (i % 50 == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    core::Env::instance().run();
    return 0;
}