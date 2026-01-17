#pragma once
#include <atomic>
#include <coroutine>
#include <concepts> // C++20
#include "aegis/net/packetPool.h"
#include "aegis/common/objectPool.h"

namespace aegis::core
{
    class PlayerActor; // 前置声明
    // 消息类型枚举
    enum MessageType : uint8_t
    {
        MSG_TYPE_BASE = 0,
        MSG_TYPE_NETWORK = 1,
        MSG_TYPE_CORO_WAKEUP = 2,
        MSG_TYPE_SESSION_CLOSED = 3,
        MSG_TYPE_SCENE_ENTER = 10,
        MSG_TYPE_SCENE_LEAVE = 11,
        MSG_TYPE_SCENE_MOVE = 12,
        MSG_TYPE_DESTROY = 20
    };

    // --- 1. 瘦基类 (无虚函数，无 vptr) ---
    // 仅用于侵入式链表的链接和类型识别
    struct ActorMessage
    {
        std::atomic<ActorMessage *> next{nullptr};
        uint8_t type_id = MSG_TYPE_BASE;

        // 禁止通过基类指针 delete，防止未定义行为 (UB)
        // 因为我们没有虚析构函数
    protected:
        ~ActorMessage() = default;

    public:
        void finalize()
        {
            delete this;
        }
    };

    // --- C++20 Concept: 约束消息必须实现 finalize ---
    template <typename T>
    concept FinalizableMessage = std::derived_from<T, ActorMessage> && requires(T m) {
        { m.finalize() } -> std::same_as<void>;
    };

    class NetworkMessage;
    using NetworkMessagePool = aegis::core::ObjectPool<NetworkMessage, 100000>;

    // --- 2. 具体消息类型 ---

    // 网络消息
    struct NetworkMessage : public ActorMessage
    {
        aegis::net::PooledPacket pkt;
        int session_id = 0;

        NetworkMessage(aegis::net::PooledPacket &&p, int sid)
        {
            type_id = MSG_TYPE_NETWORK;
            pkt = std::move(p);
            session_id = sid;
        }

        // [非虚函数] 甚至可以标记为 inline
        void finalize()
        {
            // 归还给对象池
            // Pool 内部会处理 reset，这里不需要手动析构
            NetworkMessagePool::instance().release(this);
        }

        // 对象池 reset 接口
        void reset(aegis::net::PooledPacket &&p, int sid)
        {
            next.store(nullptr, std::memory_order_relaxed);
            pkt = std::move(p);
            session_id = sid;
        }
    };

    // 协程唤醒消息
    struct CoroutineWakeupMsg : public ActorMessage
    {
        std::coroutine_handle<> handle;

        explicit CoroutineWakeupMsg(std::coroutine_handle<> h) : handle(h)
        {
            type_id = MSG_TYPE_CORO_WAKEUP;
        }

        void finalize()
        {
            // 对于非池化对象，必须显式 delete 自身
            // 这里 delete this 是安全的，因为我们在派生类上下文中
            delete this;
        }
    };

    // 会话关闭消息
    struct SessionClosedMsg : public ActorMessage
    {
        int session_id;
        SessionClosedMsg(int sid) : session_id(sid)
        {
            type_id = MSG_TYPE_SESSION_CLOSED;
        }

        void finalize()
        {
            delete this;
        }
    };

    struct SceneEnterMsg : public ActorMessage
    {
        PlayerActor *player;
        float x, y;

        SceneEnterMsg(PlayerActor *p, float px, float py)
            : player(p), x(px), y(py)
        {
            type_id = MSG_TYPE_SCENE_ENTER;
        }
        void finalize()
        {
            delete this;
        }
    };

    struct SceneLeaveMsg : public ActorMessage
    {
        uint64_t entityId;
        SceneLeaveMsg(uint64_t id) : entityId(id)
        {
            type_id = MSG_TYPE_SCENE_LEAVE;
        }
        void finalize()
        {
            delete this;
        }
    };

    struct SceneMoveMsg : public ActorMessage
    {
        uint64_t entityId;
        float oldX, oldY;
        float newX, newY;

        SceneMoveMsg(uint64_t id, float ox, float oy, float nx, float ny)
            : entityId(id), oldX(ox), oldY(oy), newX(nx), newY(ny)
        {
            type_id = MSG_TYPE_SCENE_MOVE;
        }
        void finalize()
        {
            delete this;
        }
    };

    struct ActorDestroyMsg : public ActorMessage
    {
        ActorDestroyMsg() { type_id = MSG_TYPE_DESTROY; } // 需要在常量定义里加一个
        void finalize() { delete this; }
    };

} // namespace aegis::core