# 设计决策记录

> 记录技术选型的权衡、重大重构的原因、以及踩过的坑。

---

## 1. 为什么选择 io_uring + C++20 协程

**对比 epoll 方案**:
- epoll: 每次收发至少 2 次 syscall（`epoll_wait` + `read/write`），大量小包场景开销显著
- io_uring: 通过 SQ/CQ 共享环形缓冲区，批量提交/收割，单次 syscall 处理多个 IO

**对比传统回调**:
- 回调嵌套 → 控制流离散 → 难以维护
- C++20 协程 → 线性代码 + RAII 异常安全 → 业务代码写起来像同步

**代价**:
- Linux only（io_uring 绑定）
- C++20 协程无标准库支持（`task.h` 全部自研）
- 每个 `co_await` 可能触发一次堆分配（coroutine frame）

---

## 2. Actor 模型 vs 共享内存锁

**为什么选 Actor**:
- 游戏服务器的天然隔离单位：每个玩家、场景、NPC 是独立实体
- 无共享状态 → 业务零锁 → 零死锁风险
- 消息传递天然支持跨线程通信

**ActorID 为什么设计为 64 位 (32+32)**:
- 裸指针问题：被回收后悬空 → 崩溃
- shared_ptr 问题：循环引用 + 原子引用计数开销
- 64-bit Handle：O(1) 查找，版本号防 ABA，可整体比较/哈希/传输

**代价**:
- 不适合高频共享状态场景（如全局排行榜）
- 跨 Worker 消息队列是新的竞争热点

---

## 3. 为什么把 `game/` 从 `core/` 中拆出来

**旧结构**: 所有 Actor 放在 `include/aegis/core/`

**问题**:
- GateServer 不得不在网络层上依赖游戏逻辑头文件
- 游戏逻辑的改动影响核心框架的编译
- 未来多服务（Gate/Logic/Battle）无法复用核心框架

**新结构** (`3276af8` 提交):
```
include/aegis/
├── core/    ← 框架核心（Actor/Task/Scheduler/消息基类）
├── game/    ← 游戏业务（PlayerActor/SceneActor/NpcActor/AOI/RoomManager）
└── net/     ← 网络层（Socket/Connection/Packet）
```

**结果**:
- GateServer 只依赖 `core/` + `net/` + `PlayerActor`
- 未来 LogicServer 可独立引用 `core/` + `game/`
- 编译隔离，按需包含

---

## 4. Message 系统从 `uint8_t` 升级到 `uint16_t`

**旧问题**: `type_id` 是 `uint8_t`，最多 256 种消息类型。项目扩展后已逼近上限。

**新方案**: `uint16_t` → 65536 种类型，足够容纳未来所有消息。

**联动变更**:
- `BasicMessage<Derived, TypeId>` 的模板参数也改为 `uint16_t`
- `g_message_finalizers[256]` → `g_message_finalizers[1024]`
- 单个 `message.h` 拆分为 6 个文件

**为什么不是 uint32_t**: `type_id` 存储在 `ActorMessage` 中，所有消息的基类结构体尽量小（侵入式链表节点 + type_id），追求缓存放满。

---

## 5. RPC 从同步阻塞到异步协程

**旧方案**:
```cpp
auto future = rpc_msg->promise.get_future();
dispatch_msg(target, rpc_msg);
auto res = future.get();     // ⚠️ 阻塞当前 Worker 线程！
```

**问题**: 阻塞 Worker 线程 → 该 Worker 上的所有 Actor 都无法处理消息 → 实质上的串行化。

**新方案**:
```cpp
auto res = co_await RpcCall<AssignCampRes>(target_id, rpc_msg);
```

**实现关键**:
- `RpcManager`: 每个 Worker 一个 thread_local 实例，管理所有挂起的 RPC
- `RpcAwaiter`: 自定义 `await_suspend` 注册挂起 → `await_resume` 检查结果
- `RpcMessage::Reply()`: 智能双模式——侦测 `reply_mode` 自动选择 promise 还是消息投递
- 超时机制：`await_suspend` 时注册定时器，超时恢复协程并抛异常

---

## 6. GateServer 瘦身

**旧 GateServer**: 既是网络网关，又持有游戏业务逻辑（创建场景、管理 NPC、处理房间）。

**问题**: 耦合 → gate_server.cpp 包含大量游戏逻辑头文件 → 修改场景逻辑就要重新编译整个网关。

**新架构**:
```
GateServer → 只做网络 I/O + 会话管理
GameApp    → 游戏业务启动器（创建 RoomManager、主城场景、NPC）
handler_loader → 注册业务 Handler（登录、移动、技能、营地）
```

**收益**:
- `gate_server.cpp` 精简到 220 行
- 网络层和游戏逻辑可以独立演进
- 未来 LogicServer 可以直接复用 GameApp + handler_loader

---

## 7. SyncManager 模板化 Tick

**设计动机**: SyncManager 只负责「计算谁该看到谁」、「聚合哪些数据」，不负责「怎么发送」。

```cpp
template<typename EnterLeaveFunc, typename SendBatchFunc>
void Tick(AOIGrid& aoi, EnterLeaveFunc&& onEnterLeave, SendBatchFunc&& onSendBatch);
```

**好处**:
- SyncManager 是纯算法 → 可测试（不依赖 Actor/Connection）
- SceneActor 通过回调注入发送逻辑
- 编译期模板消除虚函数调用（热路径优化）
- 不同的场景类型可以注入不同的发送回调

---

## 8. 为什么使用侵入式链表

**对比 std::list**:
- `std::list<TimerNode>`: T (24B) + prev/next (16B) = 40B，每个节点在堆上独立分配
- `IntrusiveList<TimerNode>`: prev/next 内嵌在 TimerNode 中（16B），TimerNode 来自 ObjectPool
- 内存连续 → CPU 缓存友好
- 零拷贝级联：时间轮溢出时 `splice` 整个链表，不逐个搬运节点

---

## 9. 为什么 50ms Tick 间隔

- 游戏逻辑不需要纳秒级精确
- 50ms = 20fps 的逻辑更新帧率
- 对齐大多数游戏的状态同步帧率（客户端通常 30fps 渲染，50ms 更新一次状态足够流畅）
- 时间轮 256 个槽 × 50ms = 12.8s 可容纳表一，平衡精度和槽数

---

## 10. 踩过的坑

### 10.1 io_uring + coroutine frame 生命周期

问题: 协程挂起时，io_uring 持有协程帧指针。连接关闭时，如果协程帧先析构，io_uring CQE 恢复协程会访问已释放内存。

解决: Connection 使用 `enable_shared_from_this`，`send_batch_coro` 通过 `shared_ptr<Connection> self` 延长生命周期，确保协程和 Connection 同生共死。

### 10.2 Packet 的大端序

问题: 客户端和服务端未统一字节序，测试环境中偶尔出现消息解析失败。

解决: 在 `Packet::pack_into()` 和 `OutboxBatcher::prepare_batch()` 中统一使用 `__builtin_bswap32` 做大端序转换。`Packet::msg_id()` 和 `Packet::seq_id()` 读取时也做相应转换。

### 10.3 Actor 注册表内存序

问题: `ActorRegistry::get()` 可能读到未完全初始化的 Actor 指针。

解决: `create_actor` 先构造 Actor（在局部变量），最后一步才 `actors_[idx].store(actor, release)`；`get()` 使用 `actors_[id.index].load(acquire)` → acquire-release 配对保证构造可见性。

### 10.4 NetworkMessage 池泄漏

问题: 旧版 NetworkMessage 用 `new/delete`，高频网络消息导致大量堆分配。

解决: 引入 `NetworkMessagePool (ObjectPool<NetworkMessage, 100000>)`，`finalize()` 中 `release(this)` 归还池而非 delete。
