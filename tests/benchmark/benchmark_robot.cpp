#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <iomanip>
#include <array> // 记得包含 array
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/resource.h>

// Aegis Core (假设这些头文件路径正确)
#include "aegis/core/env.h"
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include "aegis/net/socket.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"

// Proto
#include "cs_lobby.pb.h"
#include "cs_battle.pb.h"
#include "ids.pb.h"

using namespace aegis::cs::lobby;
using namespace aegis::cs::battle;
using namespace aegis;

void tune_fd_limit()
{
    struct rlimit rl;
    // 获取当前限制
    if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
    {
        perror("getrlimit");
        return;
    }

    // 将软限制提升至硬限制的水平（即 1048576）
    rl.rlim_cur = rl.rlim_max;

    if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
    {
        perror("setrlimit"); // 如果失败，通常是因为尝试超过硬限制
    }
    else
    {
        std::cout << "Successfully raised FD limit to: " << rl.rlim_cur << std::endl;
    }
}

// =========================================================
// 极简无锁延迟统计器 (Lock-Free Histogram) [已修复]
// =========================================================
class FastLatencyMonitor
{
public:
    static constexpr size_t MAX_MS = 200;

    void add_sample(double ms)
    {
        size_t idx = static_cast<size_t>(ms);
        if (idx >= MAX_MS)
            idx = MAX_MS - 1;
        buckets_[idx].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    struct SimpleStats
    {
        double avg = 0.0; // [修复] 补回 avg 字段
        size_t p99_latency = 0;
        size_t max_latency = 0;
        size_t total_count = 0;
    };

    SimpleStats report_and_reset()
    {
        SimpleStats stats;
        stats.total_count = count_.exchange(0, std::memory_order_relaxed);

        if (stats.total_count == 0)
            return stats;

        size_t threshold = static_cast<size_t>(stats.total_count * 0.99);
        size_t current_sum = 0;      // 用于找 P99
        uint64_t total_time_sum = 0; // [修复] 用于计算 Avg
        bool p99_found = false;

        for (size_t i = 0; i < MAX_MS; ++i)
        {
            int val = buckets_[i].exchange(0, std::memory_order_relaxed);

            if (val > 0)
            {
                current_sum += val;
                total_time_sum += (i * val); // 累加总耗时
                stats.max_latency = i;       // 只要有值，当前 i 就是已知的最大值

                if (!p99_found && current_sum >= threshold)
                {
                    stats.p99_latency = i;
                    p99_found = true;
                }
            }
        }

        // 计算平均值
        stats.avg = static_cast<double>(total_time_sum) / stats.total_count;
        return stats;
    }

private:
    std::array<std::atomic<int>, MAX_MS> buckets_{};
    std::atomic<size_t> count_{0};
};

// 全局指标
FastLatencyMonitor g_fast_monitor;
std::atomic<uint64_t> g_recv_count{0};
std::atomic<uint64_t> g_send_count{0};
std::atomic<uint64_t> g_error_count{0}; // [确认保留]

// =========================================================
// Robot Class
// =========================================================
class Robot : public std::enable_shared_from_this<Robot>
{
public:
    Robot(int id) : id_(id)
    {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> dist(0, 500.0f);
        x_ = dist(rng);
        y_ = dist(rng);
        std::uniform_real_distribution<float> vel(-1.0f, 1.0f);
        vx_ = vel(rng);
        vy_ = vel(rng);
    }

    core::DetachedTask start(const std::string &ip, int port)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
        {
            g_error_count++;
            co_return;
        }

        conn_ = std::make_shared<net::Connection>(net::Socket(fd));
        send_login();

        try
        {
            while (true)
            {
                auto packet = co_await conn_->read_packet();
                if (!packet)
                    break;

                g_recv_count++;
                handle_packet(*packet);
            }
        }
        catch (...)
        {
            g_error_count++;
        }
    }

    void tick(float dt)
    {
        if (!conn_)
            return;

        x_ += vx_ * 50.0f * dt;
        y_ += vy_ * 50.0f * dt;
        if (x_ <= 0 || x_ >= 500.0f)
            vx_ = -vx_;
        if (y_ <= 0 || y_ >= 500.0f)
            vy_ = -vy_;

        CSMoveReq req;
        auto *pos = req.mutable_target_pos();
        pos->set_x(x_);
        pos->set_y(y_);
        req.set_direction(0.0f);
        req.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());

        last_move_send_time_ = std::chrono::steady_clock::now();
        send_packet(ids::CS_MOVE_REQ, req);
        g_send_count++;
    }

private:
    void send_login()
    {
        LoginReq req;
        req.set_uid(10000 + id_);
        req.set_token("benchmark_token");
        send_packet(ids::CS_LOGIN_REQ, req);
    }

    template <typename T>
    void send_packet(uint32_t msg_id, const T &msg)
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (conn_)
        {
            auto pkt = net::PacketPool::instance().acquire();
            pkt->pack_into(msg_id, msg);
            conn_->send(std::move(pkt));
        }
    }

    void handle_packet(const net::Packet &pkt)
    {
        if (pkt.size() < 4)
            return;

        uint32_t net_id = 0;
        std::memcpy(&net_id, pkt.data(), 4);
        uint32_t msg_id = ntohl(net_id);

        switch (msg_id)
        {
        case ids::SC_MOVE_NTF:
        {
            auto now = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - last_move_send_time_).count();
            g_fast_monitor.add_sample(ms);
            break;
        }
        case ids::SC_ENTER_VIEW:
            break; // 优化：不解析，仅利用 g_recv_count 统计 PPS
        case ids::SC_LEAVE_VIEW:
            break;
        case ids::SC_PONG:
            break;
        case ids::SC_LOGIN_RES:
            break;
        default:
            break;
        }
    }

    int id_;
    std::shared_ptr<net::Connection> conn_;
    float x_, y_, vx_, vy_;
    std::chrono::steady_clock::time_point last_move_send_time_;
    std::mutex send_mutex_;
};

// =========================================================
// Main Loop
// =========================================================
int main()
{
    tune_fd_limit();
    Log::instance().set_level(spdlog::level::warn);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    core::Env::instance().init();
    std::vector<std::shared_ptr<Robot>> robots;
    const int ROBOT_COUNT = 800;

    std::cout << ">>> Launching robots..." << std::endl;
    for (int i = 0; i < ROBOT_COUNT; ++i)
    {
        auto r = std::make_shared<Robot>(i);
        robots.push_back(r);
        r->start("127.0.0.1", 8888);
        if (i % 50 == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::thread io_thread([]()
                          { core::Env::instance().run(); });

    auto last_tick = std::chrono::steady_clock::now();
    auto last_log = last_tick;
    uint64_t tick_count = 0;
    double max_loop_cost_ms = 0;

    // 严谨的主循环
    while (true)
    {
        auto now = std::chrono::steady_clock::now();

        // 1. 逻辑 Tick (10Hz)
        if (now - last_tick >= std::chrono::milliseconds(100))
        {
            auto tick_start = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float>(now - last_tick).count();
            last_tick = now;

            for (auto &r : robots)
                r->tick(dt);
            tick_count++;

            auto tick_end = std::chrono::high_resolution_clock::now();
            double cost_ms = std::chrono::duration<double, std::milli>(tick_end - tick_start).count();

            // 记录这一秒内的最大耗时
            if (cost_ms > max_loop_cost_ms)
                max_loop_cost_ms = cost_ms;
        }

        // 2. 日志 Tick (1Hz)
        if (now - last_log >= std::chrono::seconds(1))
        {
            auto lat = g_fast_monitor.report_and_reset();

            uint64_t qps_in = g_recv_count.exchange(0);
            uint64_t qps_out = g_send_count.exchange(0);
            uint64_t errs = g_error_count.load();
            double current_loop_cost = max_loop_cost_ms;
            max_loop_cost_ms = 0;

            std::cout << " [Client Health]" << "\n";
            std::cout << "   Loop Cost (Max): " << std::fixed << std::setprecision(2) << current_loop_cost << " ms";
            if (current_loop_cost > 80.0)
                std::cout << " [CRITICAL: Client is LAGGY]";
            else
                std::cout << " [OK]";
            std::cout << "\n";

            std::cout << "\n==================================================" << "\n";
            std::cout << " Aegis Benchmark Report (" << ROBOT_COUNT << " Bots)" << "\n";
            std::cout << "--------------------------------------------------" << "\n";
            std::cout << " [Throughput]" << "\n";
            std::cout << "   In : " << std::setw(8) << qps_in << " pkg/s" << "\n";
            std::cout << "   Out: " << std::setw(8) << qps_out << " pkg/s" << "\n";
            std::cout << "--------------------------------------------------" << "\n";

            // [修复] lat 现在有值了，count > 0 检查有效
            if (lat.total_count > 0)
            {
                std::cout << " [Latency RTT] (Samples: " << lat.total_count << ")" << "\n";
                std::cout << "   Avg : " << std::fixed << std::setprecision(2) << lat.avg << " ms" << "\n";
                std::cout << "   P99 : " << lat.p99_latency << " ms" << "\n";
                std::cout << "   Max : " << lat.max_latency << " ms" << "\n";
            }
            else
            {
                std::cout << " [Latency RTT] No samples" << "\n";
            }

            std::cout << " [Errors] Count: " << errs << "\n";
            std::cout << "==================================================" << std::endl;

            last_log = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (io_thread.joinable())
        io_thread.join();
    return 0;
}