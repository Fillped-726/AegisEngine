#pragma once
#include "aegis/core/actor.h"
#include "aegis/net/connection.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/task.h"
#include "common.pb.h"

namespace aegis::gate
{
    // [Definition] 定义会话关闭消息
    // 将其放在这里，main.cpp include 本文件后即可看到定义
    struct SessionClosedMsg : public core::ActorMessage
    {
        int reason = 0;
        SessionClosedMsg(int r = 0) : reason(r) { type_id = 0; }
    };

    class PlayerActor : public core::Actor
    {
    public:
        // Actor 持有 Connection 的 shared_ptr，防止 IO 线程断开后 Actor 还在回包导致 Crash
        explicit PlayerActor(std::shared_ptr<net::Connection> conn)
            : conn_(std::move(conn)), fd_(conn_->fd())
        {
            aegis::Log::instance().debug("PlayerActor Created | FD: {}", fd_);
        }

        ~PlayerActor()
        {
            aegis::Log::instance().debug("PlayerActor Destroyed | FD: {}", fd_);
        }

    protected:
        // --- Worker 线程执行此函数 ---
        // 这是 Actor 的主循环入口
        void handle_message(core::ActorMessage *msg) override
        {
            // 1. 网络消息处理
            if (msg->type_id == 1)
            {
                auto *net_msg = static_cast<core::NetworkMessage *>(msg);
                // 这里的 pkt 是移动语义，零拷贝
                process_packet(std::move(net_msg->pkt));
            }
            // 2. 系统消息：连接断开
            else if (msg->type_id == 0)
            {
                auto *closed_msg = static_cast<SessionClosedMsg *>(msg);
                on_session_closed(closed_msg->reason);
            }
            // 3. 其他消息 (Timer, Kick, etc.)
        }

    private:
        void on_session_closed(int reason)
        {
            aegis::Log::instance().info("[Actor] Session Closed | FD: {} | Reason: {}", fd_, reason);
            // [TODO] 在这里执行玩家下线逻辑 (保存数据到 DB, 从全局 Map 移除等)
            conn_.reset(); // 释放对连接的引用
        }

        void process_packet(net::Packet pkt)
        {
            uint32_t msg_id = pkt.msg_id();

            // 业务日志：移到了 Worker 线程打印，减轻 IO 线程负担
            // aegis::Log::instance().debug("[Worker] Processing FD: {} MsgID: {}", fd_, msg_id);

            // --- 简单的 Switch 路由 (MVP 替代 Dispatcher) ---
            switch (msg_id)
            {
            case aegis::proto::CS_LOGIN_REQ:
            {
                handle_login(pkt);
                break;
            }
            default:
            {
                aegis::Log::instance().warn("Unknown MsgID: {} from FD: {}", msg_id, fd_);
                break;
            }
            }
        }

        void handle_login(const net::Packet &pkt)
        {
            aegis::proto::LoginReq req;
            if (!pkt.parse(req))
            {
                aegis::Log::instance().warn("Parse LoginReq failed FD: {}", fd_);
                return;
            }

            aegis::Log::instance().info("[Actor] LoginReq | UID: {}", req.uid());

            // --- 构造回包 (Response) ---
            aegis::proto::LoginRes res;
            res.set_ret_code(0); // 0 = Success
            res.set_msg("Welcome to Aegis Framework!");

            // [Pack] 使用 Packet::pack 工厂方法 (自动处理 msg_id 和 protobuf 序列化)
            auto resp_pkt = net::Packet::pack(aegis::proto::SC_LOGIN_RES, res);

            // [Send] 发送回包
            conn_->send(std::move(resp_pkt));
        }

    private:
        std::shared_ptr<net::Connection> conn_;
        int fd_ = -1;
    };

} // namespace aegis::gate