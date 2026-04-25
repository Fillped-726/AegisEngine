#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <csignal>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <cstring>
#include <unistd.h>
#include <iomanip>

#include "aegis/core/scheduler.h"
#include "aegis/core/worker.h"
#include "aegis/net/socket.h"
#include "aegis/net/connection.h"
#include "aegis/net/packetPool.h"
#include "aegis/common/aegisLog.h"
#include "aegis/common/tools.h" // 假设里面有 bind_to_core 等工具函数

using namespace aegis;

// --- 压测配置参数 ---
const std::string TARGET_IP = "127.0.0.1";
const int TARGET_PORT = 8888;
const int CONCURRENCY = 16;   // 并发连接数
const int PAYLOAD_SIZE = 32;  // 报文体大小
const int DURATION_SEC = 10;  // 压测持续时间
const int PIPELINE_DEPTH = 1; // 【核心】每个连接保持在飞行状态(In-flight)的包数量

std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_total_reqs{0};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<int32_t> g_active_conn{0};

// --- 延迟直方图配置 ---
const int BUCKET_RESOLUTION_US = 10; // 每个桶 10 微秒
const int BUCKET_COUNT = 10000;      // 10000 * 10us = 100ms 最大追踪范围

// 全局无锁直方图数组 (每个 Worker 独占一行，消除跨核 Cache 伪共享)
// alignas(64) 确保每个 Worker 的数组在不同的 CPU Cache Line 上
struct alignas(64) WorkerHistogram
{
    uint64_t bins[BUCKET_COUNT] = {0};
    uint64_t out_of_bounds = 0; // 超过 100ms 的长尾怪物
};
std::vector<WorkerHistogram> g_histograms;

inline uint64_t get_now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void signal_handler(int)
{
    g_running = false;
}

// ============================================================================
// [核心协程]：Pipeline 收发死循环
// ============================================================================
core::DetachedTask client_worker(int fd)
{
    net::Socket sock(fd);
    auto conn = std::make_shared<net::Connection>(std::move(sock));
    std::string dummy_data(PAYLOAD_SIZE, 'A');

    g_active_conn.fetch_add(1, std::memory_order_relaxed);
    int worker_id = core::Worker::get_current_id();

    // 【延迟追踪利器】：环形缓冲区记录每个包的发送时间
    std::vector<uint64_t> send_timestamps(PIPELINE_DEPTH, 0);
    uint64_t send_seq = 0;
    uint64_t recv_seq = 0;

    try
    {
        // 1. Pipeline 预热：一口气把窗口塞满
        for (int i = 0; i < PIPELINE_DEPTH; ++i)
        {
            auto send_pkt = net::PacketPool::instance().acquire();
            send_pkt->alloc(PAYLOAD_SIZE);
            std::memcpy(send_pkt->mutable_data(), dummy_data.data(), PAYLOAD_SIZE);

            send_timestamps[send_seq % PIPELINE_DEPTH] = get_now_us();
            send_seq++;
            conn->send(std::move(send_pkt));
        }

        // 2. 流水线循环：收 1 个，发 1 个
        while (g_running.load(std::memory_order_relaxed))
        {
            // [收包]
            auto recv_pkt = co_await conn->read_packet();
            if (!recv_pkt)
                break;

            // [计算延迟]：严格匹配请求和响应
            uint64_t now = get_now_us();
            uint64_t rtt_us = now - send_timestamps[recv_seq % PIPELINE_DEPTH];
            recv_seq++;

            // [记录直方图 (完全无锁)]
            size_t bucket_idx = rtt_us / BUCKET_RESOLUTION_US;
            if (bucket_idx < BUCKET_COUNT)
            {
                g_histograms[worker_id].bins[bucket_idx]++;
            }
            else
            {
                g_histograms[worker_id].out_of_bounds++;
            }

            // [补发包]：维持 In-flight 数量恒定
            auto send_pkt = net::PacketPool::instance().acquire();
            send_pkt->alloc(PAYLOAD_SIZE);
            std::memcpy(send_pkt->mutable_data(), dummy_data.data(), PAYLOAD_SIZE);

            send_timestamps[send_seq % PIPELINE_DEPTH] = get_now_us();
            send_seq++;
            conn->send(std::move(send_pkt));

            // 吞吐量统计
            g_total_reqs.fetch_add(1, std::memory_order_relaxed);
            g_total_bytes.fetch_add((net::Connection::K_HEADER_SIZE + PAYLOAD_SIZE) * 2, std::memory_order_relaxed);
        }
    }
    catch (...)
    {
    }

    g_active_conn.fetch_sub(1, std::memory_order_relaxed);
}

// ============================================================================
// 建立同步连接工具
// ============================================================================
int create_connected_socket(const std::string &ip, int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        ::close(fd);
        return -1;
    }
    int opt = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    return fd;
}

// ============================================================================
// 计算并打印 Pxx 延迟数据
// ============================================================================
void print_latency_stats()
{
    uint64_t total_bins[BUCKET_COUNT] = {0};
    uint64_t total_out_of_bounds = 0;
    uint64_t total_samples = 0;

    // 1. 汇总所有 Worker 的无锁直方图
    for (const auto &wh : g_histograms)
    {
        for (int i = 0; i < BUCKET_COUNT; ++i)
        {
            total_bins[i] += wh.bins[i];
            total_samples += wh.bins[i];
        }
        total_out_of_bounds += wh.out_of_bounds;
        total_samples += wh.out_of_bounds;
    }

    if (total_samples == 0)
        return;

    // 2. 寻找分位值
    auto get_percentile = [&](double p) -> double
    {
        uint64_t target = total_samples * p;
        uint64_t sum = 0;
        for (int i = 0; i < BUCKET_COUNT; ++i)
        {
            sum += total_bins[i];
            if (sum >= target)
                return i * BUCKET_RESOLUTION_US / 1000.0; // 转换为毫秒(ms)
        }
        return BUCKET_COUNT * BUCKET_RESOLUTION_US / 1000.0; // 超过追踪范围
    };

    double p50 = get_percentile(0.50);
    double p90 = get_percentile(0.90);
    double p99 = get_percentile(0.99);
    double p999 = get_percentile(0.999);

    std::cout << "\n📊 延迟统计 (Latency Stats):\n";
    std::cout << "   样本总数: " << total_samples << " 次\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "   P50   (中位数): " << p50 << " ms\n";
    std::cout << "   P90           : " << p90 << " ms\n";
    std::cout << "   P99           : " << p99 << " ms\n";
    std::cout << "   P99.9         : " << p999 << " ms\n";
    if (total_out_of_bounds > 0)
    {
        std::cout << "   ⚠️ 警告: 有 " << total_out_of_bounds << " 次请求延迟超过 100ms!\n";
    }
}

// ============================================================================
// 主函数
// ============================================================================
int main()
{
    std::signal(SIGINT, signal_handler);
    int num_workers = std::max(1, (int)std::thread::hardware_concurrency() / 2);

    tune_fd_limit();
    aegis::Log::instance().set_level(spdlog::level::warn);

    // 初始化直方图数组
    g_histograms.resize(num_workers);

    std::cout << "🚀 Aegis Pipeline Benchmarker 启动...\n";

    std::vector<int> connected_fds;
    connected_fds.reserve(CONCURRENCY);
    for (int i = 0; i < CONCURRENCY; ++i)
    {
        int fd = create_connected_socket(TARGET_IP, TARGET_PORT);
        if (fd >= 0)
            connected_fds.push_back(fd);
        else
            break;
    }

    if (connected_fds.empty())
        return 1;

    core::Scheduler::instance().start(num_workers);

    for (size_t i = 0; i < connected_fds.size(); ++i)
    {
        int target_worker = i % num_workers;
        int fd = connected_fds[i];
        core::Scheduler::instance().get_worker(target_worker)->post_custom_task([fd]()
                                                                                { client_worker(fd); });
    }

    auto start_time = std::chrono::steady_clock::now();
    std::thread monitor([&]()
                        {
        uint64_t last_reqs = 0;
        for (int i = 0; i < DURATION_SEC && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t current_reqs = g_total_reqs.load(std::memory_order_relaxed);
            std::cout << "[实时] 在线连接: " << g_active_conn.load(std::memory_order_relaxed) 
                      << " | QPS: " << (current_reqs - last_reqs) << " req/s\n";
            last_reqs = current_reqs;
        }
        g_running = false; });
    monitor.join();

    auto end_time = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

    std::cout << "\n============================================\n";
    std::cout << "🔥 最终战报 (Duration: " << elapsed_sec << "s):\n";
    std::cout << "   平均 QPS: " << (g_total_reqs.load() / elapsed_sec) << " req/s\n";
    std::cout << "   网络吞吐: " << (g_total_bytes.load() / 1024.0 / 1024.0 / elapsed_sec) << " MB/s\n";
    print_latency_stats();
    std::cout << "============================================\n";

    core::Scheduler::instance().stop();
    return 0;
}