#include <iostream>
#include <vector>
#include <thread>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <fcntl.h>

#include "cs_lobby.pb.h"
#include "cs_battle.pb.h"

// --- 基础网络工具 ---

int connect_server()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Connect failed");
        exit(1);
    }
    return fd;
}

void send_proto(int fd, uint32_t msg_id, const google::protobuf::MessageLite &msg)
{
    std::string body = msg.SerializeAsString();
    uint32_t net_msg_id = htonl(msg_id);
    uint32_t body_len = body.size() + 4;
    uint32_t net_len = htonl(body_len);

    send(fd, &net_len, 4, 0);
    send(fd, &net_msg_id, 4, 0);
    if (!body.empty())
        send(fd, body.data(), body.size(), 0);
}

// 简单的非阻塞读取尝试，如果没有数据返回 0
uint32_t try_recv_packet(int fd, std::string &out_body)
{
    // 设置非阻塞
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    uint32_t net_len = 0;
    int n = recv(fd, &net_len, 4, MSG_PEEK); // 先偷看一眼
    if (n < 4)
        return 0; // 数据不足

    // 读长度
    recv(fd, &net_len, 4, 0);
    uint32_t len = ntohl(net_len);

    // 读 ID
    uint32_t net_msg_id = 0;
    recv(fd, &net_msg_id, 4, 0);
    uint32_t msg_id = ntohl(net_msg_id);

    // 读 Body
    int body_size = len - 4;
    if (body_size > 0)
    {
        out_body.resize(body_size);
        int total = 0;
        while (total < body_size)
        {
            n = recv(fd, out_body.data() + total, body_size - total, 0);
            if (n > 0)
                total += n;
        }
    }

    // 恢复阻塞 (可选，为了简单这里就不恢复了，下同)
    return msg_id;
}

// 阻塞读取直到收到特定消息
void wait_for_msg(int fd, uint32_t target_msg_id, const std::string &name)
{
    std::cout << "[" << name << "] Waiting for MsgID: " << target_msg_id << "..." << std::endl;

    // 恢复阻塞模式
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    while (true)
    {
        uint32_t net_len = 0;
        if (recv(fd, &net_len, 4, MSG_WAITALL) <= 0)
            exit(1);
        uint32_t len = ntohl(net_len);

        uint32_t net_id = 0;
        if (recv(fd, &net_id, 4, MSG_WAITALL) <= 0)
            exit(1);
        uint32_t msg_id = ntohl(net_id);

        std::vector<char> buf(len - 4);
        if (len > 4)
            recv(fd, buf.data(), len - 4, MSG_WAITALL);

        std::cout << "[" << name << "] Recv MsgID: " << msg_id << std::endl;

        if (msg_id == target_msg_id)
        {
            std::cout << "[" << name << "] >>> Target Msg Received! <<<" << std::endl;
            return;
        }
    }
}

// --- 主流程 ---

int main()
{
    std::cout << "=== STARTING 2-BOT SANITY TEST ===" << std::endl;

    // 1. 创建两个连接
    int fd_a = connect_server();
    std::cout << "[System] Robot A Connected (Mover)" << std::endl;

    int fd_b = connect_server();
    std::cout << "[System] Robot B Connected (Observer)" << std::endl;

    // 2. Robot A 登录 (UID 10001)
    {
        aegis::cs::lobby::LoginReq req;
        req.set_uid(10001);
        req.set_token("bot_a");
        send_proto(fd_a, 1001, req); // CS_LOGIN_REQ
    }

    // 3. Robot B 登录 (UID 10002)
    // 根据你的逻辑，Spawn位置相近，他们应该立刻能看见对方
    {
        aegis::cs::lobby::LoginReq req;
        req.set_uid(10002);
        req.set_token("bot_b");
        send_proto(fd_b, 1001, req); // CS_LOGIN_REQ
    }

    // 给服务器一点时间处理登录和 EnterView 广播
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 4. 清理一下 B 的缓冲区 (可能会收到 LoginRes 和 EnterView)
    // 简单起见，我们假设 B 现在处于干净状态或者直接在下面过滤

    // 5. Robot A 移动
    std::cout << "\n[Action] Robot A is Moving..." << std::endl;
    {
        aegis::cs::battle::CSMoveReq req;
        auto *pos = req.mutable_target_pos();

        pos->set_x(105.0f);
        pos->set_y(105.0f);

        send_proto(fd_a, 2003, req); // CS_MOVE_REQ
    }

    // 6. 验证：Robot B 是否收到了 A 的移动通知？
    // 我们期望收到 SC_MOVE_NTF (2004)
    std::cout << "[Check] Checking if Robot B receives the notification..." << std::endl;

    wait_for_msg(fd_b, 2004, "Robot B"); // 2004 = SC_MOVE_NTF

    std::cout << "\n=== TEST PASSED: Robot B saw Robot A move! ===" << std::endl;

    close(fd_a);
    close(fd_b);
    return 0;
}