#pragma once

#include <functional>
#include <unordered_map>
#include <memory>
#include <string>
#include <type_traits>
#include <concepts>

#include "aegis/core/task.h"
#include "aegis/core/actor.h"
#include "aegis/net/packet.h"
#include "aegis/common/aegisLog.h"

namespace aegis::net
{

    // 概念约束：确保 T 是一个 Protobuf Message
    template <typename T>
    concept ProtobufMessage = requires(T t, const void *data, int len) {
        { t.ParseFromArray(data, len) } -> std::convertible_to<bool>;
    };

    class Dispatcher
    {
    public:
        // 通用处理器签名 (Type Erased Handler)
        // 接受 Actor 指针和原始二进制数据
        using GenericHandler = std::function<core::Task<void>(std::shared_ptr<core::Actor>, const char *, size_t)>;

        static Dispatcher &instance()
        {
            static Dispatcher inst;
            return inst;
        }

        // --- 注册接口 (Template Magic) ---

        /**
         * @brief 注册消息处理器
         * @tparam ProtoMsg 具体的 Protobuf 消息类型 (e.g. LoginReq)
         * @param msg_id 消息 ID
         * @param func 回调函数，签名应为: Task<void>(std::shared_ptr<Actor>, const ProtoMsg&)
         */
        template <typename ProtoMsg, typename Func>
            requires ProtobufMessage<ProtoMsg>
        void register_handler(uint32_t msg_id, Func &&func)
        {
            // 创建一个 Lambda 包装器，负责 "反序列化 -> 调用业务逻辑"
            // 这个 Lambda 本身也是一个协程
            GenericHandler wrapper = [func = std::forward<Func>(func), msg_id](std::shared_ptr<core::Actor> actor, const char *data, size_t len) -> core::Task<void>
            {
                ProtoMsg msg;
                // 1. 反序列化
                // 注意：Protobuf ParseFromArray 接受 int，这里强转是安全的 (包大小有限制)
                if (!msg.ParseFromArray(data, static_cast<int>(len)))
                {
                    aegis::Log::instance().warn("Dispatcher: Parse failed. MsgID: {}", msg_id);
                    co_return;
                }

                // 2. 调用用户的强类型回调
                // 这是一个尾调用，我们 co_await 用户逻辑完成
                co_await func(actor, msg);
            };

            handlers_[msg_id] = std::move(wrapper);
        }

        // --- 分发接口 (Dispatch) ---

        /**
         * @brief 根据 Packet 查找并执行处理器
         * @note 如果找不到处理器或解析失败，会打印警告但不会崩溃
         */
        core::Task<void> dispatch(std::shared_ptr<core::Actor> actor, const Packet &pkt)
        {
            uint32_t msg_id = pkt.msg_id();

            auto it = handlers_.find(msg_id);
            if (it != handlers_.end())
            {
                const char *body = static_cast<const char *>(pkt.body_ptr());
                size_t len = pkt.body_len();

                // 执行包装好的 GenericHandler
                co_await it->second(actor, body, len);
            }
            else
            {
                // [Optional] 可以增加一个 default_handler 处理未注册消息
                aegis::Log::instance().warn("Dispatcher: No handler found for MsgID: {}", msg_id);
            }
        }

    private:
        Dispatcher() = default;

        std::unordered_map<uint32_t, GenericHandler> handlers_;
    };

} // namespace aegis::net