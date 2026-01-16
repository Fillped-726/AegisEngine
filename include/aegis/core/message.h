#pragma once
#include <atomic>
#include <memory>
#include <coroutine>
#include "aegis/net/packetPool.h"
#include "aegis/common/objectPool.h"

namespace aegis::core
{
    class NetworkMessage;
    // 对象池定义
    // 使用 ObjectPool 管理 NetworkMessage，最大容量 10万
    using NetworkMessagePool = aegis::core::ObjectPool<NetworkMessage, 100000>;
    enum : uint8_t
    {
        MSG_ID_CORO_WAKEUP = 255 // 协程唤醒消息 ID
    };
    // --- 1. 消息基类 (带虚函数) ---
    struct ActorMessage
    {
        std::atomic<ActorMessage *> next{nullptr};

        uint8_t type_id = 0;

        // 虚析构函数：保证 delete 基类指针时，子类析构函数被调用
        // 这样 unique_ptr<Packet> 就能自动释放了
        virtual ~ActorMessage() = default;

        // 【核心设计】虚函数：自我销毁
        // 让对象自己决定：是 delete 掉，还是还给对象池
        virtual void finalize()
        {
            delete this; // 默认行为：直接删除
        }
    };

    // --- 2. 网络消息 (走对象池) ---
    struct NetworkMessage : public ActorMessage
    {
        aegis::net::PooledPacket pkt; // unique_ptr，自动管理生命周期
        int session_id = 0;

        // 默认构造函数 (供对象池预分配使用)
        NetworkMessage()
        {
            type_id = 1; // 假设 MSG_NETWORK = 1
        }

        // 【新增】匹配 reset 参数的构造函数
        // 当对象池为空需要 new 新对象时，acquire 会调用此构造函数
        NetworkMessage(aegis::net::PooledPacket &&p, int sid)
        {
            type_id = 1;
            pkt = std::move(p);
            session_id = sid;
        }

        // 覆盖 finalize：将自己归还给池子
        void finalize() override
        {
            // 注意：这里需要先把 pkt 等资源 reset 或者是 pool 的 release 内部处理
            // 通常 Pool 的 release 只是把指针放回去，不会析构对象
            // 所以对象的状态会在下一次 acquire 时的 reset 中被覆盖

            // 归还给自己所属的池子
            NetworkMessagePool::instance().release(this);
        }

        // 对象池复用接口 (当从池中取出旧对象时调用)
        void reset(aegis::net::PooledPacket &&p, int sid)
        {
            next.store(nullptr, std::memory_order_relaxed);
            // type_id 理论上不会变，但为了保险可以重置
            // type_id = 1;
            pkt = std::move(p);
            session_id = sid;
        }
    };

    // [New] 协程唤醒消息
    struct CoroutineWakeupMsg : public ActorMessage
    {
        std::coroutine_handle<> handle;

        explicit CoroutineWakeupMsg(std::coroutine_handle<> h)
            : handle(h)
        {
            type_id = MSG_ID_CORO_WAKEUP; // 使用特殊 ID 区分
        }
    };

    // [Definition] 定义会话关闭消息
    struct SessionClosedMsg : public ActorMessage
    {
        int session_id = 0;
        SessionClosedMsg(int sid = 0) : session_id(sid) { type_id = 0; }
    };

} // namespace aegis::core