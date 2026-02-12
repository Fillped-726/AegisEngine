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
    // 概念约束：确保 T 是一个 Protobuf Message (用于网络包)
    template <typename T>
    concept ProtobufMessage = requires(T t, const void *data, int len) {
        { t.ParseFromArray(data, len) } -> std::convertible_to<bool>;
    };

    class Dispatcher
    {
    public:
        // ---------------------------------------------------------------------
        // 模式 1: 网络消息处理器 (处理二进制 Packet)
        // ---------------------------------------------------------------------
        using NetHandler = std::function<core::Task<void>(core::Actor *, const char *, size_t)>;

        // ---------------------------------------------------------------------
        // 模式 2: RPC/内部消息处理器 (处理内存对象指针)
        // [新增] 接受 void* 指针，因为我们在分发前不知道具体的 struct 类型
        // ---------------------------------------------------------------------
        using RpcHandler = std::function<core::Task<void>(core::Actor *, const void *)>;

        static Dispatcher &instance()
        {
            static Dispatcher inst;
            return inst;
        }

        // =====================================================================
        // 注册接口 A: 网络消息 (Protobuf 反序列化)
        // =====================================================================
        template <typename ProtoMsg, typename Func>
            requires ProtobufMessage<ProtoMsg>
        void register_handler(uint32_t msg_id, Func &&func)
        {
            NetHandler wrapper = [func = std::forward<Func>(func), msg_id](core::Actor *actor, const char *data, size_t len) -> core::Task<void>
            {
                ProtoMsg msg;
                if (!msg.ParseFromArray(data, static_cast<int>(len)))
                {
                    aegis::Log::instance().warn("Dispatcher: Parse failed. MsgID: {}", msg_id);
                    co_return;
                }
                co_await func(actor, msg);
            };

            net_handlers_[msg_id] = std::move(wrapper);
        }

        // =====================================================================
        // 注册接口 B: RPC 消息 (直接指针转换) -> [这是你缺少的]
        // =====================================================================
        /**
         * @brief 注册 RPC 处理器
         * @tparam RpcMsgType 我们定义的 RpcMessage<...> 包装类型
         * @param msg_id 业务 ID (如 SS_CREATE_ROOM_REQ)
         */
        template <typename RpcMsgType, typename Func>
        void register_rpc(uint32_t msg_id, Func &&func)
        {
            // 创建 Lambda 包装器
            // 这里不需要反序列化，只需要安全的 static_cast
            RpcHandler wrapper = [func = std::forward<Func>(func)](core::Actor *actor, const void *msg_ptr) -> core::Task<void>
            {
                // 安全转换：我们信任 Dispatcher 的路由表是正确的
                const auto *msg = static_cast<const RpcMsgType *>(msg_ptr);

                // 直接调用业务逻辑
                co_await func(actor, *msg);
            };

            rpc_handlers_[msg_id] = std::move(wrapper);
        }

        // =====================================================================
        // 分发接口
        // =====================================================================

        // 1. 网络分发 (处理 Packet)
        core::Task<void> dispatch(core::Actor *actor, const Packet &pkt)
        {
            uint32_t msg_id = pkt.msg_id();
            auto it = net_handlers_.find(msg_id);
            if (it != net_handlers_.end())
            {
                co_await it->second(actor, static_cast<const char *>(pkt.data() + kPacketMsgHeader), pkt.size() - kPacketMsgHeader);
            }
            else
            {
                aegis::Log::instance().warn("Dispatcher: No NetHandler for MsgID: {}", msg_id);
            }
        }

        // 2. RPC 分发 (处理 RPC Message 对象) -> [这是你需要调用的]
        // 注意：这里需要传入具体的业务 MsgID (比如 3001)，因为 ActorMessage 只有 TypeID (比如 50)
        core::Task<void> dispatch_rpc(core::Actor *actor, uint32_t msg_id, const void *msg_ptr)
        {
            auto it = rpc_handlers_.find(msg_id);
            if (it != rpc_handlers_.end())
            {
                // 直接传递指针，零拷贝
                co_await it->second(actor, msg_ptr);
            }
            else
            {
                aegis::Log::instance().error("Dispatcher: No RpcHandler for MsgID: {}", msg_id);
            }
        }

    private:
        Dispatcher() = default;

        std::unordered_map<uint32_t, NetHandler> net_handlers_; // 存储网络回调
        std::unordered_map<uint32_t, RpcHandler> rpc_handlers_; // 存储 RPC 回调
    };

} // namespace aegis::net