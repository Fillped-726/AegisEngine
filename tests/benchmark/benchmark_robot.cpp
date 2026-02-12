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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

// Aegis Core
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

// =========================================================
// 指标采集器：计算 P50, P99 (线程安全)
// =========================================================
class LatencyMonitor
{
public:
    void add_sample(double ms)
    {
        // 简单自旋锁或互斥锁，压测场景下 10k QPS 的锁竞争可以接受
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back(ms);
    }

    struct Results
    {
        double avg = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
        size_t count = 0;
    };

    Results report_and_clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty())
            return {};

        // 排序以获取百分位数
        std::sort(samples_.begin(), samples_.end());

        double sum = 0;
        for (double s : samples_)
            sum += s;

        Results r;
        r.count = samples_.size();
        r.avg = sum / r.count;
        r.p50 = samples_[size_t(r.count * 0.50)];
        r.p90 = samples_[size_t(r.count * 0.90)];
        r.p99 = samples_[size_t(r.count * 0.99)];
        r.p999 = samples_[size_t(r.count * 0.999)];
        r.max = samples_.back();

        samples_.clear();
        return r;
    }

private:
    std::vector<double> samples_;
    std::mutex mutex_;
};

// 全局指标
LatencyMonitor g_latency_monitor;
std::atomic<uint64_t> g_recv_count{0};
std::atomic<uint64_t> g_send_count{0};
std::atomic<uint64_t> g_recv_bytes{0};
std::atomic<uint64_t> g_enter_view_count{0};
std::atomic<uint64_t> g_error_count{0};

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
                g_recv_bytes += packet->size();
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

        // 简单的反弹移动模拟
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
        // 方向同步
        req.set_direction(0.0f);
        // 时间戳 (用于服务端延迟补偿计算)
        req.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());

        last_move_send_time_ = std::chrono::steady_clock::now();

        // [SSP Fix] 使用枚举值，不要写死 2001 (那现在是 PING 了！)
        // 正确应该是 CS_MOVE_REQ (2003)
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
            net::PooledPacket pkt = std::make_unique<net::Packet>();

            pkt->pack_into(msg_id, msg);

            // 4. 发送
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

        // [SSP Fix] 使用 switch-case 和枚举，清晰且不易错
        switch (msg_id)
        {
        case ids::SC_MOVE_NTF: // 2004 (旧代码是 1002)
        {
            // 计算 RTT
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - last_move_send_time_);
            g_latency_monitor.add_sample(duration.count() / 1000.0);
            break;
        }
        case ids::SC_ENTER_VIEW: // 2005 (旧代码是 1003)
        {
            SCEnterViewNtf ntf;
            if (ntf.ParseFromArray(pkt.data() + 4, pkt.size() - 4))
            {
                g_enter_view_count += ntf.entities_size();
            }
            break;
        }
        case ids::SC_LEAVE_VIEW: // 2006
        {
            // 处理离开视野逻辑...
            break;
        }
        case ids::SC_PONG: // 2002
        {
            break;
        }
        case ids::SC_LOGIN_RES: // 1002
        {
            // 登录成功
            break;
        }
        default:
            // std::cout << "Unknown MsgID: " << msg_id << std::endl;
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
    Log::instance().set_level(spdlog::level::warn);
    // 关闭控制台缓冲，防止打印错乱
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    core::Env::instance().init();
    std::vector<std::shared_ptr<Robot>> robots;
    const int ROBOT_COUNT = 500;

    std::cout << ">>> Launching robots..." << std::endl;
    for (int i = 0; i < ROBOT_COUNT; ++i)
    {
        auto r = std::make_shared<Robot>(i);
        robots.push_back(r);
        r->start("127.0.0.1", 8888);
        if (i % 50 == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // 后台启动 IO 线程
    std::thread io_thread([]()
                          { core::Env::instance().run(); });

    auto last_tick = std::chrono::steady_clock::now();
    auto last_log = last_tick;
    uint64_t tick_count = 0;

    // 严谨的主循环
    while (true)
    {
        auto now = std::chrono::steady_clock::now();

        // 1. 逻辑 Tick (10Hz)
        if (now - last_tick >= std::chrono::milliseconds(100))
        {
            float dt = std::chrono::duration<float>(now - last_tick).count();
            last_tick = now;

            for (auto &r : robots)
                r->tick(dt);
            tick_count++;
        }

        // 2. 日志 Tick (1Hz)
        if (now - last_log >= std::chrono::seconds(1))
        {
            // 获取并重置计数器
            auto lat = g_latency_monitor.report_and_clear();
            uint64_t qps_in = g_recv_count.exchange(0);
            uint64_t qps_out = g_send_count.exchange(0);
            double bw_mb = g_recv_bytes.exchange(0) / 1024.0 / 1024.0;
            uint64_t ev = g_enter_view_count.exchange(0);
            uint64_t errs = g_error_count.load();

            // 格式化输出
            std::cout << "\n==================================================" << "\n";
            std::cout << " Aegis Benchmark Report (" << ROBOT_COUNT << " Bots)" << "\n";
            std::cout << "--------------------------------------------------" << "\n";
            std::cout << " [Throughput]" << "\n";
            std::cout << "   In : " << std::setw(8) << qps_in << " pkg/s" << "\n";
            std::cout << "   Out: " << std::setw(8) << qps_out << " pkg/s" << "\n";
            std::cout << "   BW : " << std::fixed << std::setprecision(2) << bw_mb << " MB/s" << "\n";
            std::cout << " [Logic]" << "\n";
            std::cout << "   EnterView: " << ev << " entities/s" << "\n";
            std::cout << "--------------------------------------------------" << "\n";

            if (lat.count > 0)
            {
                std::cout << " [Latency RTT] (Samples: " << lat.count << ")" << "\n";
                std::cout << "   Avg : " << std::fixed << std::setprecision(2) << lat.avg << " ms" << "\n";
                std::cout << "   P50 : " << lat.p50 << " ms" << "\n";
                std::cout << "   P90 : " << lat.p90 << " ms" << "\n";
                std::cout << "   P99 : " << lat.p99 << " ms" << "\n";
                std::cout << "   Max : " << lat.max << " ms" << "\n";
            }
            else
            {
                std::cout << " [Latency RTT] No samples (Check MsgID 1002)" << "\n";
            }

            std::cout << " [Errors] Count: " << errs << "\n";
            std::cout << "==================================================" << std::endl;

            last_log = now;
        }

        // 避免空转烧 CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (io_thread.joinable())
        io_thread.join();
    return 0;
}