# 消息系统 — Message 类型层次与生命周期

> 对应目录: `include/aegis/core/message/`

---

## 1. 架构概述

**关键版本变更**: `uint8_t type_id` → **`uint16_t type_id`** (扩容)，从单文件 `message.h` 拆分为 6 个模块文件。

```
message_base.h       ← ActorMessage 基类 + BasicMessage CRTP + Finalizer 注册表
message_id.h         ← MessageType 枚举 (所有 type_id 常量)
message_net.h        ← NetworkMessage / SessionClosedMsg / ForwardPacketMsg
message_rpc.h        ← RpcMessage<T> / RpcResponseMsg / RPC 消息类型定义
message_scene.h      ← SceneEnterMsg / SceneLeaveMsg / SceneMoveMsg / SkillCastMsg
message_lifecycle.h  ← CoroutineWakeupMsg / ActorDestroyMsg / ActorDiedMsg / PoisonPillMsg
```

---

## 2. 基类层次

### 2.1 ActorMessage (`message_base.h`)

```cpp
struct ActorMessage {
    std::atomic<ActorMessage*> next{nullptr};  // 侵入式链表节点
    uint16_t type_id = MSG_TYPE_BASE;          // 消息类型 ID
    virtual ~ActorMessage() = default;
};
```

### 2.2 BasicMessage CRTP (`message_base.h`)

```cpp
template<typename Derived, uint16_t TypeId>
struct BasicMessage : public ActorMessage {
    BasicMessage() {
        type_id = TypeId;
        // 静态注册 finalizer（只执行一次）
        static bool registered = []{
            g_message_finalizers[TypeId] = [](ActorMessage *msg) {
                static_cast<Derived*>(msg)->finalize();
            };
            return true;
        }();
    }
    void finalize() { delete static_cast<Derived*>(this); }
};
```

**Finalizer 注册表** (`g_message_finalizers[1024]`):
- 类型安全的虚函数替代方案：无需 `virtual finalize()` 虚表
- 模板在编译期生成类型特定的销毁逻辑
- Actor::free_message() 通过 type_id 索引 finalizer
- 支持 NetworkMessage 这种不走 delete 而走 ObjectPool::release 的特化

---

## 3. MessageType 枚举 (`message_id.h`)

```cpp
enum MessageType : uint16_t {
    // ── 基础 ──
    MSG_TYPE_BASE            = 0,
    MSG_TYPE_NETWORK         = 1,     // 网络消息（客户端发来的 protobuf 包）
    MSG_TYPE_CORO_WAKEUP     = 2,     // 协程唤醒
    MSG_TYPE_SESSION_CLOSED  = 3,     // 连接断开

    // ── 场景消息 (10~19) ──
    MSG_TYPE_SCENE_ENTER     = 10,    // 进入场景
    MSG_TYPE_SCENE_LEAVE     = 11,    // 离开场景
    MSG_TYPE_SCENE_MOVE      = 12,    // 移动
    MSG_TYPE_SCENE_SKILL_CAST = 13,   // 技能释放
    MSG_TYPE_REQ_SCENE_SNAPSHOT = 14, // 请求场景快照

    // ── 生命周期 (20~29) ──
    MSG_TYPE_DESTROY         = 20,    // 销毁 Actor
    MSG_TYPE_ACTOR_DIED      = 21,    // Actor 遗言通知
    MSG_TYPE_POISON_PILL     = 22,    // 毒药丸：真死兜底

    // ── RPC (50~59) ──
    MSG_TYPE_RPC_CREATE_ROOM = 50,    // 创建房间 RPC
    MSG_TYPE_RPC_TERMINATE_ROOM = 51, // 销毁房间 RPC
    MSG_TYPE_RPC_ASSIGN_CAMP = 52,    // 分配营地 RPC

    // ── 转发 (60~69) ──
    MSG_TYPE_FORWARD_PACKET  = 60,    // 跨 Actor 包转发
    MSG_TYPE_REBIND_CONNECTION = 61,  // 顶号重连

    // ── 系统 (70~79) ──
    MSG_TYPE_RPC_RESPONSE    = 70,    // RPC 响应（协程模式）
    MSG_TYPE_CAMP_PLAYER_COUNT = 71,  // 营地人数上报
};
```

---

## 4. 消息类型详解

### 4.1 网络消息 (`message_net.h`)

```cpp
// 来自客户端的网络包
struct NetworkMessage : public ActorMessage {
    PooledPacket pkt;
    int session_id = 0;
    void finalize() { NetworkMessagePool::instance().release(this); }
};
```
- 使用专用 `ObjectPool<NetworkMessage, 100000>` 减少分配
- `reset(pkt, sid)` 方法支持池复用

```cpp
// 连接断开通知
struct SessionClosedMsg : public BasicMessage<SessionClosedMsg, MSG_TYPE_SESSION_CLOSED> {
    int session_id;
};
```

```cpp
// 跨 Worker 包转发（广播用，共享 buffer 避免拷贝）
struct ForwardPacketMsg : public BasicMessage<ForwardPacketMsg, MSG_TYPE_FORWARD_PACKET> {
    uint32_t msg_id;
    uint32_t seq_id;
    std::shared_ptr<std::string> shared_buf;  // ⭐ 共享 Buffer
};
```

```cpp
// 顶号重连
struct RebindConnectionMsg : public BasicMessage<RebindConnectionMsg, MSG_TYPE_REBIND_CONNECTION> {
    std::shared_ptr<net::Connection> new_conn;
};
```

### 4.2 RPC 消息 (`message_rpc.h`)

```cpp
// 协程 RPC 响应
struct RpcResponseMsg : public BasicMessage<RpcResponseMsg, MSG_TYPE_RPC_RESPONSE> {
    uint64_t rpc_id;
    void *result_storage;
    std::function<void()> deleter;
};
```

```cpp
// 泛型 RPC 消息（模板，编译期生成特化）
template<typename ReqT, typename ResT, uint16_t TypeId>
struct RpcMessage : public ActorMessage {
    ReqT req;
    mutable std::promise<ResT> promise;       // 旧模式：同步阻塞
    mutable uint64_t rpc_id = 0;              // 新模式：协程异步
    RpcReplyMode reply_mode = RpcReplyMode::Promise;
    ActorID requester_id_;                     // 发起者 ActorID

    void Reply(const ResT& res) const;  // 智能回复：自动选择模式
};
```

**回复双模式**:
- `Promise` 模式：通过 `promise.set_value(res)` — 对应旧式 `future.get()` 阻塞调用
- `Coroutine` 模式：投递 `RpcResponseMsg` 到发起者所在 Worker — 对应 `co_await RpcCall<>()`

已定义的 RPC 消息类型:

```cpp
using RPCCreateRoomMsg     = RpcMessage<CreateRoomReq, CreateRoomRes,     MSG_TYPE_RPC_CREATE_ROOM>;
using RPCTerminateRoomMsg  = RpcMessage<TerminateRoomReq, TerminateRoomRes, MSG_TYPE_RPC_TERMINATE_ROOM>;
using RPCAssignCampMsg     = RpcMessage<AssignCampReq, AssignCampRes,     MSG_TYPE_RPC_ASSIGN_CAMP>;
```

### 4.3 场景消息 (`message_scene.h`)

```cpp
struct SceneEnterMsg      : BasicMessage<SceneEnterMsg, MSG_TYPE_SCENE_ENTER>     { ActorID actor_id; uint64_t player_id; float x, y; };
struct SceneLeaveMsg      : BasicMessage<SceneLeaveMsg, MSG_TYPE_SCENE_LEAVE>     { ActorID actor_id; uint64_t player_id; };
struct SceneMoveMsg       : BasicMessage<SceneMoveMsg, MSG_TYPE_SCENE_MOVE>       { ActorID actor_id; uint64_t player_id; uint32_t aoi_grid_index; float newX, newY; uint8_t direction; };
struct ReqSceneSnapshotMsg : BasicMessage<ReqSceneSnapshotMsg, MSG_TYPE_REQ_SCENE_SNAPSHOT> { uint64_t player_id; };
struct SceneSkillCastMsg  : BasicMessage<SceneSkillCastMsg, MSG_TYPE_SCENE_SKILL_CAST> { ActorID actor_id; uint32_t skill_id; uint64_t target_uid; float target_x, target_y; };
```

### 4.4 生命周期消息 (`message_lifecycle.h`)

```cpp
struct CoroutineWakeupMsg  : BasicMessage<CoroutineWakeupMsg, MSG_TYPE_CORO_WAKEUP>   { std::coroutine_handle<> handle; };
struct ActorDestroyMsg     : BasicMessage<ActorDestroyMsg, MSG_TYPE_DESTROY>           {};
struct ActorDiedMsg        : BasicMessage<ActorDiedMsg, MSG_TYPE_ACTOR_DIED>           { ActorID deceased_id; int reason; };
struct PoisonPillMsg       : BasicMessage<PoisonPillMsg, MSG_TYPE_POISON_PILL>         {};
```

---

## 5. 消息生命周期

| 消息类型 | 分配方式 | 处理方式 | 释放方式 |
|---------|---------|---------|---------|
| NetworkMessage | ObjectPool<NetworkMessage> | PlayerActor::handle_message → Dispatcher | ObjectPool::release (finalize) |
| RpcMessage (Promise) | `new` | 目标 Actor 处理 → msg.Reply() | promise.set_value 后 `delete` |
| RpcMessage (Coroutine) | `new` | 目标 Actor 处理 → msg.Reply() | RpcResponseMsg 投递 → requestor finalize |
| RpcResponseMsg | `new` | PlayerActor → RpcManager::consume | 手动 deleter |
| SceneEnter/Leave/Move | `new` | SceneActor::handle_message | `delete this` (BasicMessage 默认) |
| SessionClosedMsg | `new` | PlayerActor → on_session_closed | `delete this` |
| ForwardPacketMsg | `new` | PlayerActor → send_buffer | `delete this` |
| ActorDiedMsg | `new` | 父 Actor 监控 | `delete this` |

---

## 6. 跨 Worker 消息投递

```cpp
// dispatch_msg() — 通用投递工具 (actor_utils.cpp)
void dispatch_msg(Actor *actor, ActorMessage *msg) {
    if (actor->push(msg)) {
        if (actor->worker_id() == current_worker->id()) {
            current_worker->dispatch_local(actor);
        } else {
            target_worker->post_cross_core_task(actor);
        }
    }
}
```

- **同一 Worker**: 直接 `dispatch_local()` — 入队到 Actor → 不跨线程
- **不同 Worker**: `post_cross_core_task()` → moodycamel 无锁队列 → eventfd 唤醒
