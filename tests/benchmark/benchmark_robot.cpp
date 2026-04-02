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
#include <array>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/resource.h>

// Aegis Core
#include "aegis/core/scheduler.h"
#include "aegis/core/worker.h"
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
// [注意]: 确保你在 cs_battle.proto 中已经加上了 SCMoveNtfBatch
// 并且在 ids.proto 中加上了 SC_MOVE_NTF_BATCH = xxx;

using namespace aegis::cs::lobby;
using namespace aegis::cs::battle;
using namespace aegis;

void tune_fd_limit()
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
    {
        perror("getrlimit");
        return;
    }

    rl.rlim_cur = rl.rlim_max;

    if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
    {
        perror("setrlimit");
    }
    else
    {
        std::cout << "Successfully raised FD limit to: " << rl.rlim_cur << std::endl;
    }
}

// =========================================================
// 极简无锁延迟统计器 (Lock-Free Histogram)
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
        double avg = 0.0;
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
        size_t current_sum = 0;
        uint64_t total_time_sum = 0;
        bool p99_found = false;

        for (size_t i = 0; i < MAX_MS; ++i)
        {
            int val = buckets_[i].exchange(0, std::memory_order_relaxed);

            if (val > 0)
            {
                current_sum += val;
                total_time_sum += (i * val);
                stats.max_latency = i;

                if (!p99_found && current_sum >= threshold)
                {
                    stats.p99_latency = i;
                    p99_found = true;
                }
            }
        }

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
std::atomic<uint64_t> g_error_count{0};

// =========================================================
// Robot Class
// =========================================================
class Robot : public std::enable_shared_from_this<Robot>
{
public:
    Robot(int id, int num_workers) : id_(id), worker_id_(id_ % num_workers)
    {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> dist(0, 500.0f);
        x_ = dist(rng);
        y_ = dist(rng);
        std::uniform_real_distribution<float> vel(-1.0f, 1.0f);
        vx_ = vel(rng);
        vy_ = vel(rng);
    }

    int worker_id() const
    {
        return worker_id_;
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
        // [修改]: 替换为新的批量通知协议 ID
        case ids::SC_MOVE_NTF:
        {
            // 面试高光：真正的延迟必须用心跳包测，移动包包含了服务器端 Tick 的缓冲等待时间 (Jitter)
            // 在此仅作大致的网络与Tick合并耗时评估
            auto now = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - last_move_send_time_).count();
            g_fast_monitor.add_sample(ms);
            break;
        }
        case ids::SC_ENTER_VIEW:
            break;
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
    int worker_id_;
    std::shared_ptr<net::Connection> conn_;
    float x_, y_, vx_, vy_;
    std::chrono::steady_clock::time_point last_move_send_time_;
};

// =========================================================
// Main Loop
// =========================================================
int main()
{
    tune_fd_limit();
    Log::instance().set_level(spdlog::level::warn);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    int num_client_workers = 4;
    core::Scheduler::instance().start(num_client_workers);

    std::vector<std::shared_ptr<Robot>> robots;

    const int ROBOT_COUNT = 100;

    std::cout << ">>> Launching robots..." << std::endl;
    for (int i = 0; i < ROBOT_COUNT; ++i)
    {
        auto r = std::make_shared<Robot>(i, num_client_workers);
        robots.push_back(r);
        auto *worker = core::Scheduler::instance().get_worker(r->worker_id());
        worker->post_custom_task([r]()
                                 { r->start("127.0.0.1", 8888); });
        if (i % 50 == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto last_tick = std::chrono::steady_clock::now();
    auto last_log = last_tick;
    uint64_t tick_count = 0;
    double max_loop_cost_ms = 0;

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
            {
                auto *worker = core::Scheduler::instance().get_worker(r->worker_id());
                worker->post_custom_task([r, dt]()
                                         { r->tick(dt); });
            }
            tick_count++;

            auto tick_end = std::chrono::high_resolution_clock::now();
            double cost_ms = std::chrono::duration<double, std::milli>(tick_end - tick_start).count();

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

    core::Scheduler::instance().stop();
    return 0;
}