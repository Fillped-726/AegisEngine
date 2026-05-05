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
#include <algorithm>

#include "aegis/core/scheduler.h"
#include "aegis/core/worker.h"
#include "aegis/net/connection.h"
#include "aegis/net/socket.h"
#include "aegis/net/connection.h"
#include "aegis/net/packetPool.h"
#include "aegis/common/aegisLog.h"
#include "aegis/common/tools.h"

using namespace aegis;
using namespace aegis::core;
using namespace aegis::net;

// --- 配置 ---
const std::string TARGET_IP = "127.0.0.1";
const int TARGET_PORT = 8888;
const int DURATION_SEC = 5;

// --- 测试矩阵 ---
const std::vector<int> PAYLOAD_SIZES = {32, 128, 512, 1024, 4096};
const std::vector<int> CONCURRENCIES = {1, 4, 16, 64, 256};
const std::vector<int> PIPELINE_DEPTHS = {1, 64};  // 两种场景

// 全局停止标志（仅用于 Ctrl+C）
std::atomic<bool> g_global_stop{false};
std::atomic<uint64_t> g_total_reqs{0};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<int32_t> g_active_conn{0};

// --- 延迟直方图 (10us 精度, 100ms 范围) ---
const int BUCKET_RESOLUTION_US = 10;
const int BUCKET_COUNT = 10000;

struct alignas(64) WorkerHistogram
{
    uint64_t bins[BUCKET_COUNT] = {0};
    uint64_t out_of_bounds = 0;
};
std::vector<WorkerHistogram> g_histograms;

inline uint64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void signal_handler(int) { g_global_stop = true; }

// ============================================================================
// 核心压测协程（支持 Pipeline Depth）
// ============================================================================
core::DetachedTask client_worker(int fd, int payload_size, int pipeline_depth,
                                     const std::atomic<bool> &stop_flag)
{
    net::Socket sock(fd);
    auto conn = std::make_shared<net::Connection>(std::move(sock));
    std::string dummy_data(payload_size, 'X');

    g_active_conn.fetch_add(1, std::memory_order_relaxed);
    int worker_id = core::Worker::get_current_id();

    // 环形时间戳缓冲区（记录每个 in-flight 包的发送时间）
    std::vector<uint64_t> send_ts(pipeline_depth, 0);
    uint64_t send_seq = 0;
    uint64_t recv_seq = 0;

    try
    {
        // 预热：填满 Pipeline
        for (int i = 0; i < pipeline_depth; ++i)
        {
            auto pkt = net::PacketPool::instance().acquire();
            pkt->alloc(payload_size);
            std::memcpy(pkt->mutable_data(), dummy_data.data(), payload_size);
            send_ts[send_seq % pipeline_depth] = now_us();
            send_seq++;
            conn->send(std::move(pkt));
        }

        // 主循环：收一个，发一个（维持恒定 In-flight）
        while (!stop_flag.load(std::memory_order_relaxed) &&
               !g_global_stop.load(std::memory_order_relaxed))
        {
            auto recv = co_await conn->read_packet();
            if (!recv) break;

            // 计算 RTT（匹配对应包的发送时间）
            uint64_t rtt = now_us() - send_ts[recv_seq % pipeline_depth];
            recv_seq++;

            size_t idx = rtt / BUCKET_RESOLUTION_US;
            if (idx < BUCKET_COUNT)
                g_histograms[worker_id].bins[idx]++;
            else
                g_histograms[worker_id].out_of_bounds++;

            // 补发一个（维持 Pipeline 深度）
            auto pkt = net::PacketPool::instance().acquire();
            pkt->alloc(payload_size);
            std::memcpy(pkt->mutable_data(), dummy_data.data(), payload_size);
            send_ts[send_seq % pipeline_depth] = now_us();
            send_seq++;
            conn->send(std::move(pkt));

            g_total_reqs.fetch_add(1, std::memory_order_relaxed);
            g_total_bytes.fetch_add((Connection::K_HEADER_SIZE + payload_size) * 2,
                                    std::memory_order_relaxed);
        }
    }
    catch (...) {}

    g_active_conn.fetch_sub(1, std::memory_order_relaxed);
}

// ============================================================================
// 工具函数
// ============================================================================
int dial(const std::string &ip, int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

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
// 单组测试
// ============================================================================
struct TestResult
{
    int payload;
    int conn;
    int depth;
    double qps;
    double mbps;
    double p50, p90, p99, p999;
};

TestResult run_test(int payload_size, int concurrency, int pipeline_depth, int workers)
{
    // 每组独立的停止标志
    auto group_stop = std::make_shared<std::atomic<bool>>(false);

    // 清零直方图
    for (auto &wh : g_histograms)
        std::memset(wh.bins, 0, sizeof(wh.bins));
    g_total_reqs.store(0, std::memory_order_relaxed);
    g_total_bytes.store(0, std::memory_order_relaxed);

    // 建连
    std::vector<int> fds;
    for (int i = 0; i < concurrency; ++i)
    {
        int fd = dial(TARGET_IP, TARGET_PORT);
        if (fd >= 0) fds.push_back(fd);
        else break;
    }
    if (fds.empty()) return {};

    // 分发到各 Worker（传入私有 stop_flag）
    for (size_t i = 0; i < fds.size(); ++i)
    {
        int w = i % workers;
        Scheduler::instance().get_worker(w)->post_custom_task(
            [fd = fds[i], payload_size, pipeline_depth, group_stop]()
            { client_worker(fd, payload_size, pipeline_depth, *group_stop); });
    }

    // 运行
    std::this_thread::sleep_for(std::chrono::seconds(DURATION_SEC));

    // 停止本组所有协程
    group_stop->store(true, std::memory_order_relaxed);

    // 等待协程退出
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 优雅关闭：先 shutdown 发送端，让服务端读到 EOF
    for (int fd : fds)
    {
        ::shutdown(fd, SHUT_WR);
    }
    // 等 200ms 让服务端处理 EOF 并关闭连接
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 再关闭 fd
    for (int fd : fds) ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    double elapsed = DURATION_SEC;

    // 汇总直方图
    uint64_t total_samples = 0;
    WorkerHistogram hist;
    for (const auto &wh : g_histograms)
    {
        for (int i = 0; i < BUCKET_COUNT; ++i)
        {
            hist.bins[i] += wh.bins[i];
            total_samples += wh.bins[i];
        }
        total_samples += wh.out_of_bounds;
    }

    auto p = [&](double perc) -> double
    {
        uint64_t target = total_samples * perc;
        uint64_t sum = 0;
        for (int i = 0; i < BUCKET_COUNT; ++i)
        {
            sum += hist.bins[i];
            if (sum >= target) return i * BUCKET_RESOLUTION_US / 1000.0;
        }
        return BUCKET_COUNT * BUCKET_RESOLUTION_US / 1000.0;
    };

    TestResult r;
    r.payload = payload_size;
    r.conn = concurrency;
    r.depth = pipeline_depth;
    r.qps = g_total_reqs.load() / elapsed;
    r.mbps = g_total_bytes.load() / 1024.0 / 1024.0 / elapsed;
    r.p50 = p(0.50);
    r.p90 = p(0.90);
    r.p99 = p(0.99);
    r.p999 = p(0.999);
    return r;
}

// ============================================================================
// 打印测试矩阵
// ============================================================================
void print_matrix(const std::vector<TestResult> &results, int depth)
{
    std::cout << "\n";
    std::cout << "────────────────────────────────────────────────────────────────\n";
    std::cout << "  Pipeline Depth = " << depth
              << "  (Depth=1: 纯延迟 | Depth>=8: 饱和吞吐)\n";
    std::cout << "────────────────────────────────────────────────────────────────\n\n";

    for (int ps : PAYLOAD_SIZES)
    {
        std::cout << "── Payload: " << ps << " bytes ─────────────────────\n";
        std::cout << std::left << std::setw(8) << "Conns"
                  << std::setw(16) << "QPS"
                  << std::setw(14) << "Throughput"
                  << "  Latency (P50/P90/P99/P99.9)\n";
        std::cout << std::string(75, '-') << "\n";

        for (const auto &r : results)
        {
            if (r.payload != ps) continue;
            std::cout << std::left << std::setw(8) << r.conn
                      << std::right << std::setw(14) << std::fixed << std::setprecision(0) << r.qps << " qps"
                      << std::setw(12) << std::fixed << std::setprecision(1) << r.mbps << " MB/s"
                      << "   " << std::fixed << std::setprecision(2)
                      << r.p50 << "/" << r.p90 << "/" << r.p99 << "/" << r.p999 << " ms\n";
        }
        std::cout << "\n";
    }
}

void print_summary(const std::vector<TestResult> &results)
{
    // 按 Depth 分组汇总
    for (int depth : PIPELINE_DEPTHS)
    {
        double peak_qps = 0, best_p99 = 999, peak_mbps = 0;
        int peak_conn = 0, peak_payload = 0;
        for (const auto &r : results)
        {
            if (r.depth != depth) continue;
            if (r.qps > peak_qps)
            {
                peak_qps = r.qps;
                peak_conn = r.conn;
                peak_payload = r.payload;
            }
            if (r.p99 < best_p99 && r.qps > 0) best_p99 = r.p99;
            if (r.mbps > peak_mbps) peak_mbps = r.mbps;
        }

        std::cout << "  Depth=" << depth
                  << "  Peak " << std::fixed << std::setprecision(0) << peak_qps << " qps"
                  << "  (Payload=" << peak_payload << "B, Conns=" << peak_conn << ")\n";
        std::cout << "  Depth=" << depth
                  << "  Best P99=" << std::fixed << std::setprecision(2) << best_p99 << " ms\n";
        std::cout << "  Depth=" << depth
                  << "  Peak Throughput=" << std::fixed << std::setprecision(1) << peak_mbps << " MB/s\n";
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
    aegis::Log::instance().set_level(spdlog::level::err);
    g_histograms.resize(num_workers);

    std::cout << "AegisEngine Benchmark Client\n";
    std::cout << "  Workers:    " << num_workers << "\n";
    std::cout << "  Target:     " << TARGET_IP << ":" << TARGET_PORT << "\n";
    std::cout << "  Payloads:   ";
    for (int s : PAYLOAD_SIZES) std::cout << s << "B ";
    std::cout << "\n  Concurrency: ";
    for (int c : CONCURRENCIES) std::cout << c << " ";
    std::cout << "\n  Pipeline:   ";
    for (int d : PIPELINE_DEPTHS) std::cout << d << " ";
    std::cout << "\n  Duration:   " << DURATION_SEC << "s/test\n";
    std::cout << "  NOTE: Start benchmark_echo first!\n\n";

    // 检查服务端
    int probe = dial(TARGET_IP, TARGET_PORT);
    if (probe < 0)
    {
        std::cerr << "[ERROR] Cannot connect. Start benchmark_echo first.\n";
        return 1;
    }
    ::close(probe);

    Scheduler::instance().start(num_workers);

    // 运行测试矩阵
    std::vector<TestResult> results;
    int total = PAYLOAD_SIZES.size() * CONCURRENCIES.size() * PIPELINE_DEPTHS.size();
    int done = 0;

    for (int depth : PIPELINE_DEPTHS)
    {
        for (int ps : PAYLOAD_SIZES)
        {
            for (int conc : CONCURRENCIES)
            {
                done++;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

                std::cout << "[" << done << "/" << total << "] "
                          << "Depth=" << depth << " Payload=" << ps << "B Conns=" << conc << " ... " << std::flush;

                auto r = run_test(ps, conc, depth, num_workers);
                if (r.qps > 0)
                {
                    results.push_back(r);
                    std::cout << r.qps << " qps, P99=" << r.p99 << "ms\n";
                }
                else
                {
                    std::cout << "FAILED\n";
                }
            }
        }
    }

    Scheduler::instance().stop();

    // 输出：先 Depth=1，再 Depth=64
    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "  AegisEngine Benchmark Report\n";
    std::cout << "  Mode: Echo  |  " << TARGET_IP << ":" << TARGET_PORT
              << "  |  Duration: " << DURATION_SEC << "s/test\n";
    std::cout << "================================================================================\n\n";

    for (int depth : PIPELINE_DEPTHS)
        print_matrix(results, depth);

    std::cout << "── Summary ─────────────────────────────────────────────\n";
    print_summary(results);
    std::cout << "================================================================================\n";
}
