#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

// Core
#include "aegis/core/env.h"
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include "aegis/net/socket.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"

// Proto
#include "common.pb.h"
#include "scene.pb.h"

using namespace aegis;

// =========================================================
// Configuration
// =========================================================
const std::string SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 8888;
const int ROBOT_COUNT = 600;      // 机器人数量
const int MOVE_INTERVAL_MS = 100; // 移动间隔 (100ms = 10Hz)
const float MAP_SIZE = 500.0f;    // 地图大小

// Stats (原子计数器)
std::atomic<uint64_t> g_recv_count{0};
std::atomic<uint64_t> g_recv_bytes{0};
std::atomic<uint64_t> g_enter_view_count{0}; // 看到的实体总数

// =========================================================
// Robot Class
// =========================================================
class Robot : public std::enable_shared_from_this<Robot>
{
public:
    Robot(int id) : id_(id)
    {
        // 随机出生点
        static std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> dist(0, MAP_SIZE);
        x_ = dist(rng);
        y_ = dist(rng);

        // 随机移动方向
        std::uniform_real_distribution<float> vel(-1.0f, 1.0f);
        vx_ = vel(rng);
        vy_ = vel(rng);
    }

    // 启动机器人
    core::DetachedTask start()
    {
        // 1. 建立连接 (Blocking for simplicity in startup)
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP.c_str(), &addr.sin_addr);

        if (connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
        {
            aegis::Log::instance().error("[Robot-{}] Connect Failed", id_);
            co_return;
        }

        // 2. 托管给 Connection (io_uring)
        conn_ = std::make_shared<net::Connection>(net::Socket(fd));

        // 3. 发送登录包
        send_login();

        // 4. 开启接收循环 (协程)
        try
        {
            while (true)
            {
                auto packet = co_await conn_->read_packet();
                if (!packet)
                    break;

                // 统计
                g_recv_count++;
                g_recv_bytes += packet->payload_.size();

                // 解析消息 (简单的 ID 路由)
                handle_packet(*packet);
            }
        }
        catch (...)
        {
        }

        aegis::Log::instance().warn("[Robot-{}] Disconnected", id_);
    }

    // 心跳/移动 Tick (由主线程驱动)
    void tick(float dt)
    {
        if (!conn_)
            return;

        // 简单的反弹移动逻辑
        x_ += vx_ * speed_ * dt;
        y_ += vy_ * speed_ * dt;

        // 碰到边界反弹
        if (x_ <= 0 || x_ >= MAP_SIZE)
            vx_ = -vx_;
        if (y_ <= 0 || y_ >= MAP_SIZE)
            vy_ = -vy_;

        // 构造移动包
        protocol::CSMoveReq req;
        auto *pos = req.mutable_target_pos();
        pos->set_x(x_);
        pos->set_y(y_);

        // 发送 (MsgID: 2001)
        send_packet(2001, req);
    }

private:
    void send_login()
    {
        protocol::LoginReq req;
        req.set_uid(10000 + id_); // 模拟 UID
        req.set_token("robot_token");
        send_packet(1001, req); // MsgID: 1001
    }

    template <typename T>
    void send_packet(uint32_t msg_id, const T &msg)
    {
        if (conn_)
        {
            auto pkt = net::Packet::pack(msg_id, msg);
            conn_->send(std::move(pkt));
        }
    }

    void handle_packet(const net::Packet &pkt)
    {
        // 简单的解析逻辑，只关心 AOI 广播
        // 注意：Packet 包含 Header(8 bytes) + Body
        // 我们需要跳过 Header 解析 MsgID 和 Body

        if (pkt.payload_.size() < 4)
            return;

        uint32_t net_id;
        std::memcpy(&net_id, pkt.payload_.data(), 4);
        uint32_t msg_id = ntohl(net_id);

        // 1003: SCEnterViewNtf
        if (msg_id == 1003)
        {
            protocol::SCEnterViewNtf ntf;
            if (ntf.ParseFromArray(pkt.payload_.data() + 4, pkt.payload_.size() - 4))
            {
                g_enter_view_count += ntf.entities_size();
            }
        }
        // 1002: SCMoveNtf (可以统计收到多少移动包)
        else if (msg_id == 1002)
        {
            // do nothing, just counting bandwidth
        }
    }

private:
    int id_;
    std::shared_ptr<net::Connection> conn_;

    // 运动状态
    float x_, y_;
    float vx_, vy_;
    float speed_ = 50.0f; // 速度 50 unit/s
};

// =========================================================
// Main Driver
// =========================================================

int main()
{
    // 1. 初始化环境
    Log::instance().set_level(spdlog::level::warn);
    core::Env::instance().init();

    std::vector<std::shared_ptr<Robot>> robots;
    robots.reserve(ROBOT_COUNT);

    std::cout << ">>> Launching " << ROBOT_COUNT << " robots..." << std::endl;

    // 2. 批量创建并连接机器人
    for (int i = 0; i < ROBOT_COUNT; ++i)
    {
        auto robot = std::make_shared<Robot>(i);
        robots.push_back(robot);
        robot->start();

        // 避免瞬间连接风暴，稍微 sleep
        if (i % 50 == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << ">>> All Robots Launched. Starting Move Loop." << std::endl;

    // 3. 启动 IO 线程 (在后台运行)
    std::thread io_thread([]()
                          { core::Env::instance().run(); });

    // 4. 主线程作为 "Tick Driver" (模拟客户端帧循环)
    // 每 100ms 驱动所有机器人移动一次
    auto last_time = std::chrono::steady_clock::now();
    auto last_log_time = last_time;

    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> dt = now - last_time;

        // 限制 Tick 频率 (10Hz)
        if (dt.count() < 0.1f)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        last_time = now;

        // 驱动移动
        for (auto &robot : robots)
        {
            robot->tick(dt.count());
        }

        // 统计日志 (每秒输出一次)
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 1)
        {
            uint64_t count = g_recv_count.exchange(0);
            uint64_t bytes = g_recv_bytes.exchange(0);
            uint64_t view = g_enter_view_count.exchange(0);

            double mbps = (double)bytes / 1024.0 / 1024.0;

            std::cout << "[Stats] "
                      << "QPS: " << count << " | "
                      << "Bandwidth: " << mbps << " MB/s | "
                      << "EnterView: " << view
                      << std::endl;

            last_log_time = now;
        }
    }

    if (io_thread.joinable())
        io_thread.join();
    return 0;
}