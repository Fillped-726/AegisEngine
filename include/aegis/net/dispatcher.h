#pragma once
#include <functional>
#include <map>
#include <memory>
#include <iostream>
#include "aegis/core/task.h"
#include "aegis/net/connection.h"
#include "aegis/net/packet.h"

namespace aegis::net
{

    class Dispatcher
    {
    public:
        // 定义通用的处理函数类型：输入 Connection 和 Packet
        using PacketHandler = std::function<core::Task<void>(Connection &, const Packet &)>;

        // --- 注册接口 (核心魔法) ---
        // T 是具体的 Protobuf 类型 (如 LoginReq)
        // Handler 是你的业务逻辑函数: (Connection& conn, const T& msg) -> Task<void>
        template <typename T, typename Handler>
        void register_handler(uint32_t msg_id, Handler &&handler)
        {

            // 我们创建一个 Lambda 包装器，把通用的 Packet 转换成具体的 T
            auto wrapper = [func = std::forward<Handler>(handler)](Connection &conn, const Packet &packet) -> core::Task<void>
            {
                T msg;
                // 1. 自动解析
                if (!packet.parse(msg))
                {
                    std::cerr << "Dispatcher: Parse error for MsgID " << packet.msg_id() << std::endl;
                    co_return;
                }
                // 2. 调用业务逻辑
                co_await func(conn, msg);
            };

            // 存入 map
            handlers_[msg_id] = std::move(wrapper);
        }

        // --- 分发接口 ---
        core::Task<void> dispatch(Connection &conn, const Packet &packet)
        {
            uint32_t msg_id = packet.msg_id();

            auto it = handlers_.find(msg_id);
            if (it != handlers_.end())
            {
                // 找到对应的处理函数，调用它
                co_await it->second(conn, packet);
            }
            else
            {
                std::cerr << "Dispatcher: Unknown MsgID " << msg_id << std::endl;
            }
        }

    private:
        std::map<uint32_t, PacketHandler> handlers_;
    };

} // namespace aegis::net