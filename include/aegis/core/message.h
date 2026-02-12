#pragma once
#include <atomic>
#include <coroutine>
#include <concepts>
#include <future> // for std::promise
#include "aegis/net/packetPool.h"
#include "aegis/common/objectPool.h"
#include "ss_bridge.pb.h"
#include "aegis/core/actor_registry.h"

namespace aegis::core
{
    class PlayerActor;

    enum MessageType : uint8_t
    {
        MSG_TYPE_BASE = 0,
        MSG_TYPE_NETWORK = 1,
        MSG_TYPE_CORO_WAKEUP = 2,
        MSG_TYPE_SESSION_CLOSED = 3,

        // --- 场景/业务消息 ---
        MSG_TYPE_SCENE_ENTER = 10,
        MSG_TYPE_SCENE_LEAVE = 11,
        MSG_TYPE_SCENE_MOVE = 12,

        // --- 生命周期消息 ---
        MSG_TYPE_DESTROY = 20,    // 命令 Actor 销毁 (Input)
        MSG_TYPE_ACTOR_DIED = 21, // 遗言：通知父节点我已销毁 (Output)

        // --- RPC ---
        MSG_TYPE_RPC_CREATE_ROOM = 50,
        MSG_TYPE_RPC_TERMINATE_ROOM = 51,
    };

    // --- 1. 基础消息头 (保持不变) ---
    struct ActorMessage
    {
        std::atomic<ActorMessage *> next{nullptr};
        uint8_t type_id = MSG_TYPE_BASE;

    protected:
        ~ActorMessage() = default; // 非虚析构
    };

    // --- 2. 核心模版工具 (CRTP) ---

    /**
     * @brief 标准消息模板
     * 自动处理 type_id 赋值和安全的 delete this
     * @tparam Derived 派生类类型 (CRTP)
     * @tparam TypeId 消息 ID
     */
    template <typename Derived, uint8_t TypeId>
    struct BasicMessage : public ActorMessage
    {
        BasicMessage()
        {
            type_id = TypeId;
        }

        // 编译期静态多态，替代虚函数
        void finalize()
        {
            // CRTP 关键：强转回派生类指针再删除，确保调用派生类析构函数
            static_assert(sizeof(Derived) > 0, "Derived must be a complete type");
            delete static_cast<Derived *>(this);
        }
    };

    // --- 3. 具体消息定义 (大幅简化) ---

    // 协程唤醒消息
    struct CoroutineWakeupMsg : public BasicMessage<CoroutineWakeupMsg, MSG_TYPE_CORO_WAKEUP>
    {
        std::coroutine_handle<> handle;
        explicit CoroutineWakeupMsg(std::coroutine_handle<> h) : handle(h) {}
    };

    // 会话关闭消息
    struct SessionClosedMsg : public BasicMessage<SessionClosedMsg, MSG_TYPE_SESSION_CLOSED>
    {
        int session_id;
        explicit SessionClosedMsg(int sid) : session_id(sid) {}
    };

    // 场景进入
    struct SceneEnterMsg : public BasicMessage<SceneEnterMsg, MSG_TYPE_SCENE_ENTER>
    {
        core::ActorID actor_id;
        uint64_t player_id; // 业务 ID (UID)
        float x, y;
        SceneEnterMsg(core::ActorID pid, uint64_t uid, float px, float py) : actor_id(pid), player_id(uid), x(px), y(py)
        {
            type_id = MSG_TYPE_SCENE_ENTER;
        }
    };

    // 场景离开
    struct SceneLeaveMsg : public BasicMessage<SceneLeaveMsg, MSG_TYPE_SCENE_LEAVE>
    {
        core::ActorID actor_id;
        uint64_t player_id; // 业务 ID (UID)
        explicit SceneLeaveMsg(core::ActorID pid, uint64_t uid) : actor_id(pid), player_id(uid)
        {
            type_id = MSG_TYPE_SCENE_LEAVE;
        }
    };

    // 场景移动
    struct SceneMoveMsg : public BasicMessage<SceneMoveMsg, MSG_TYPE_SCENE_MOVE>
    {
        core::ActorID actor_id;
        uint64_t player_id; // 业务 ID (UID)
        float oldX, oldY;
        float newX, newY;

        SceneMoveMsg(core::ActorID id, uint64_t uid, float ox, float oy, float nx, float ny)
            : actor_id(id), player_id(uid), oldX(ox), oldY(oy), newX(nx), newY(ny)
        {
            type_id = MSG_TYPE_SCENE_MOVE;
        }
    };

    // 遗言消息：当 Actor 返回 Dead 状态后，由调度器发送给 Parent
    struct ActorDiedMsg : public BasicMessage<ActorDiedMsg, MSG_TYPE_ACTOR_DIED>
    {
        core::ActorID deceased_id; // 死者的 ID
        int reason;                // 死因 (0: 正常退出, 1: 异常崩溃, etc.)

        ActorDiedMsg(core::ActorID id, int r = 0)
            : deceased_id(id), reason(r)
        {
        }
    };

    // 销毁消息
    struct ActorDestroyMsg : public BasicMessage<ActorDestroyMsg, MSG_TYPE_DESTROY>
    {
    };

    // --- 4. 特殊消息处理 ---
    /**
     * @brief 通用 RPC 消息模板
     * @tparam ReqT 请求体类型 (Proto struct)
     * @tparam ResT 响应体类型 (Proto struct)
     * @tparam TypeId 消息 ID
     */
    template <typename ReqT, typename ResT, uint8_t TypeId>
    struct RpcMessage : public ActorMessage
    {
        // 1. 数据区
        ReqT req;
        mutable std::promise<ResT> promise;

        // 2. 构造函数
        explicit RpcMessage(const ReqT &request) : req(request)
        {
            type_id = TypeId;
        }

        // [新增] 无参构造 / 内部构造 (用于 GateServer 手动触发)
        RpcMessage()
        {
            type_id = TypeId;
        }

        // 3. 禁止拷贝 (Promise 不可拷贝)
        RpcMessage(const RpcMessage &) = delete;
        RpcMessage &operator=(const RpcMessage &) = delete;

        // 4. finalize 实现
        // 因为 RpcMessage 就是最终的具体类型 (Concrete Type)，
        // 所以直接 delete this 是安全的，会正确调用 ~ReqT() 和 ~promise()
        void finalize()
        {
            delete this;
        }

        // 5. 快速回包辅助
        void Reply(const ResT &res) const
        {
            promise.set_value(res);
        }
    };

    // --- 使用别名定义具体消息 (极其简洁) ---

    // 假设 Proto 定义在 aegis::ss::bridge 下
    using RPCCreateRoomMsg = RpcMessage<
        aegis::ss::bridge::CreateRoomReq,
        aegis::ss::bridge::CreateRoomRes,
        MSG_TYPE_RPC_CREATE_ROOM>;

    using RPCTerminateRoomMsg = RpcMessage<
        aegis::ss::bridge::TerminateRoomReq,
        aegis::ss::bridge::TerminateRoomRes,
        MSG_TYPE_RPC_TERMINATE_ROOM>;

    // [网络消息]：由于涉及对象池，逻辑特殊，建议单独定义或使用专门的 Pool 模板
    // 为了极致性能和清晰度，保留手动控制，但利用继承减少 type_id 赋值
    class NetworkMessage;
    using NetworkMessagePool = aegis::core::ObjectPool<NetworkMessage, 100000>;

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

        // 自定义 finalize，不走 delete，而是走 release
        void finalize()
        {
            NetworkMessagePool::instance().release(this);
        }

        void reset(aegis::net::PooledPacket &&p, int sid)
        {
            next.store(nullptr, std::memory_order_relaxed);
            type_id = MSG_TYPE_NETWORK;
            pkt = std::move(p);
            session_id = sid;
        }
    };

    // --- Concept 校验 ---
    // 确保所有消息都符合要求（编译期检查）
    template <typename T>
    concept FinalizableMessage = std::derived_from<T, ActorMessage> && requires(T m) {
        { m.finalize() } -> std::same_as<void>;
    };

} // namespace aegis::core