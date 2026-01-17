#include <iostream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

// 【关键】直接包含你项目中生成的 protobuf 头文件
// 确保 CMake 包含了生成的目录，通常是 build/proto 或类似路径
#include "common.pb.h"

using aegis::protocol::LoginReq;

// 简单的字节序转换辅助 (Big Endian)
void write_uint32_be(char *buf, uint32_t val)
{
    uint32_t be_val = htonl(val);
    std::memcpy(buf, &be_val, 4);
}

uint32_t read_uint32_be(const char *buf)
{
    uint32_t be_val;
    std::memcpy(&be_val, buf, 4);
    return ntohl(be_val);
}

int main()
{
    // 1. 创建 Socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("Socket creation failed");
        return 1;
    }

    // 2. 连接服务器
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8888);
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0)
    {
        perror("Invalid address");
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("Connection failed");
        return 1;
    }
    std::cout << "[Client] Connected to Gate Server!" << std::endl;

    // 3. 构造 Protobuf 消息
    LoginReq req;
    req.set_uid(10086);
    req.set_token("Sakura_SSP_Candidate");

    std::string body;
    req.SerializeToString(&body); // 序列化

    // 4. 封包 (Length + MsgID + Body)
    uint32_t msg_id = 1001;
    uint32_t body_len = body.size();
    // 假设你的协议头是：[Length(4)] + [MsgID(4)] + [Body]
    // 注意：这里的 Length 通常指 (MsgID长度 + Body长度) 或者 仅 Body长度，
    // 看你 Connection.cpp 里的具体实现。
    // 这里假设 Length = 4 (MsgID) + BodySize
    uint32_t packet_len = 4 + body_len;

    std::vector<char> buffer(4 + packet_len);

    // 写入 Length
    write_uint32_be(buffer.data(), packet_len);
    // 写入 MsgID
    write_uint32_be(buffer.data() + 4, msg_id);
    // 写入 Body
    std::memcpy(buffer.data() + 8, body.data(), body_len);

    // 5. 发送
    send(sock, buffer.data(), buffer.size(), 0);
    std::cout << "[Client] Sent LoginReq (Size: " << buffer.size() << ")" << std::endl;

    // 6. 简单的接收回显 (阻塞读取)
    char recv_buf[1024] = {0};
    int n = read(sock, recv_buf, 1024);
    if (n > 0)
    {
        std::cout << "[Client] Received " << n << " bytes response." << std::endl;
        // 如果想解析回包：
        // 1. 读前4字节长度
        // 2. 读前4字节 MsgID
        // 3. LoginResp resp; resp.ParseFromArray(...)
    }

    close(sock);
    return 0;
}