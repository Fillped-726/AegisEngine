# Aegis Engine 核心架构文档

> 版本: v0.2.0 · 最后更新: 2026-04-25

---

## 1. 设计哲学

### 1.1 三个核心约束

| 约束 | 理由 | 影响 |
|:---|:---|:---|
| **零锁业务代码** | Actor 模型天然避免共享状态 | 业务层没有 `std::mutex`，没有死锁风险 |
| **零系统调用热路径** | io_uring SQPOLL 内核态轮询 | 高频 IO 不陷入内核，CPU 占用率极低 |
| **零分配消息传递** | 网络包从创建到销毁全程对象池 | GC 友好，无运行时抖动 (Jitter) |

### 1.2 为什么不用 Boost.Asio / libuv

| 方案 | 问题 |
|:---|:---|
| Boost.Asio | 模板爆炸，编译慢，ABI 脆弱；Proactor 模式不够纯粹 |
| libuv | Reactor 模型，每次 IO 至少 2 次系统调用 (epoll_wait + read/write) |
| 自研 io_uring | 彻底掌控 SQE/CQE 生命周期，支持 `IORING_OP_READV`/`WRITEV` 零拷贝 |

---

## 2. 分层架构

### 2.1 五层架构全景

```
Phase 5: 服务层 ─── GateServer / RoomManager / SceneActor
    │
    ▼ 消息投递
Phase 4: Actor 框架 ─── Actor 基类 / 无锁邮箱 / ActorRegistry
    │
    ▼ 协程调度
Phase 3: 核心运行时 ─── Scheduler / Worker / Task<T> / TimeWheel
    │
    ▼ IO 提交
Phase 2: 网络传输层 ─── Acceptor / Connection / Packet / OutboxBatcher
    │
    ▼ 系统调用
Phase 1: 基础设施 ─── ObjectPool / SpinLock / IntrusiveList / WSQueue
```

### 2.2 层间通信契约

| 接口 | 方向 | 调用者 → 被调用者 | 同步点 |
|:---|:---|:---|:---|
| `co_await read_packet()` | L2→L3 | Worker 协程 → Connection | 协程挂起/恢复 |
| `dispatch_to_actor()` | L5→L4 | GateServer → Actor::push() | 无锁入队 |
| `ActorRegistry::get()` | L4→L3 | Handler → ActorRegistry | atomic load (acquire) |
| `Worker::post_custom_task()` | L5→L3 | accept_loop → 目标 Worker | 无锁队列 tail push |
| `Scheduler::dispatch()` | L3→L3 | 新 Actor → Round-Robin Worker | 原子计数器 + 队列 |

---

## 3. 核心数据结构关系图

```
Scheduler (单例)
  ├── workers_[] (vector<unique_ptr<Worker>>)
  │     ├── ring_ (io_uring 实例)
  │     ├── local_queue_ (Chase-Lev WorkStealingQueue)
  │     ├── timer_wheel_ (HierarchicalTimeWheel)
  │     └── thread_ (std::thread)
  │
  └── rr_counter_ (atomic<uint64_t>, Round-Robin 分发)

ActorRegistry (单例)
  ├── actors_[] (array<atomic<Actor*>, 65536>)
  ├── versions_[] (array<atomic<uint32_t>, 65536>)
  └── free_indices_ (ConcurrentQueue)

Actor
  ├── id_ (ActorID: 64-bit)
  ├── mailbox_ (Intrusive MPSC Queue: next_ 原子指针)
  ├── parent_id_ (ActorID, 监管树)
  └── worker_id_ (int, 绑定的 Worker)

ActorMessage (消息基类)
  ├── next_ (atomic<ActorMessage*>, 侵入式链表节点)
  ├── type_id_ (uint8_t)
  └── finalize() (CRTP 自动注册的析构逻辑)

Connection
  ├── socket_ (RAII Socket)
  ├── rx_buffer_ (vector<char>, 动态扩容)
  ├── rx_len_ (已缓存字节数)
  ├── outbox_ (Outbox: vector<PooledPacket>)
  └── batcher_ (OutboxBatcher: iovec 聚合)
```

---

## 4. 数据流：从网线到业务逻辑

```
[TCP 数据到达]
    │
    ▼
io_uring CQ (内核填充完成事件)
    │
    ▼
Worker::process_io_completions()
    │
    ▼ 获取完整帧 (K_HEADER_SIZE=4B)
Connection::read_packet() co_await
    │
    ▼ 拆包成功 → PooledPacket
handle_session() 循环体
    │
    ▼ dispatch_to_actor()
Actor::push(msg) — MPSC 无锁入队
    │
    ▼ Worker 调度
Actor 的 do_work() 被调用
    │
    ▼ 根据 type_id 查找 Handler
handler_loader 静态反射 → Lambda
    │
    ▼ 解析 Protobuf Body
msg->parse<LoginReq>()
    │
    ▼ 业务逻辑执行
[玩家进入场景 · AOI 注册 · 广播通知]
```

### 4.1 关键路径代码定位

| 步骤 | 文件名 | 关键函数/类 |
|:---|:---|:---|
| io_uring CQ 处理 | `ring.cpp` | `Ring::reap_completions()` |
| 协程管理 Worker 循环 | `worker.cpp` | `Worker::run()` |
| TCP 数据包拆包 | `connection.cpp` | `Connection::read_packet()` |
| 网络消息投递 Actor | `gate_server.cpp` | `GateServer::dispatch_to_actor()` |
| Actor 消息邮箱入队 | `actor.h` | `Actor::push()` |
| Actor 业务处理 | `actor.cpp` | `Actor::do_work()` |
| Handler 消息映射 | `handler_loader.h` | `#define REGISTER_HANDLER` |

---

## 5. 调度器详解

### 5.1 Worker 生命周期

```
Scheduler::start(N)
  └── for i in 0..N:
        ├── Worker::Worker(i)      # 创建 Worker 实例
        ├── std::thread([&] {      # 启动系统线程
        │     worker.run();
        │   })
        └── 线程绑定:
              pthread_setaffinity_np(core i)

Worker::run() 主循环:
  while (running):
    1. io_uring reap completions           # 收割完成 IO
    2. timer_wheel.tick()                  # 驱动时间轮
    3. process_local_queue()               # 处理本地任务
    4. try_steal(other_worker)             # 工作窃取
    5. io_uring submit新SQE (如果有待发)   # 提交新 IO
```

### 5.2 工作窃取策略

Chase-Lev Work-Stealing Deque 实现:

```
Worker A (繁忙)            Worker B (空闲)
┌──────────────┐          ┌──────────────┐
│  bottom ─────┤          │  bottom      │
│   Task 3     │          │              │
│   Task 2     │  steal←  │              │
│   Task 1     │  top→    │              │
└──────────────┘          └──────────────┘
```

- **Owner (Worker A):** LIFO 从 bottom push/pop（缓存友好）
- **Thief (Worker B):** FIFO 从 top steal（原子操作 `compare_exchange_weak`）
- **安全:** 仅在 bottom >= top 时进行 steal，避免数据竞争

### 5.3 跨 Worker 通信

```
Worker A                     Worker B
  │                            │
  │  post_cross_core_task()    │
  │  ──────────────────────►   │
  │       actor_ptr            │
  │                            │
  │        (最终)              │
  │                      Worker B 的 do_work 调用 actor
```

`post_cross_core_task()` 将 Actor 指针写入目标 Worker 的 `cross_core_queue_` (MoodyCamel ConcurrentQueue)，然后通过 `io_uring IORING_OP_NOP` 唤醒目标 Worker 的 `io_uring_wait_cqe()`。

---

## 6. Actor 系统详解

### 6.1 ActorID 设计

```cpp
union ActorID {
    struct {
        uint32_t index;    // Registry 数组下标 (0~65534)
        uint32_t version;  // 版本号，每次复用 +1 (初始 1)
    } parts;
    uint64_t raw;          // 作为 uint64_t 整体操作
};
```

- 作为网络传输和哈希的 64-bit 不透明值
- `raw == 0` 表示无效 ID
- `compare_exchange_weak` 做 CAS 更新时可直接比较 `raw`

### 6.2 注册表 (ActorRegistry) 工作流

```
create_actor<T>(args...):
  1. free_indices_.try_dequeue(idx)     ← 取空闲槽
  2. ver = versions_[idx]               ← 读版本号
  3. id = {.index=idx, .version=ver}
  4. actor = T::create(id, args...)     ← 构造
  5. actor->set_worker_id(current_worker)
  6. actors_[idx].store(actor, release) ← 发布
  7. return id

get(id):
  1. actor = actors_[id.index].load(acquire)
  2. if actor == null → return null
  3. if actor->id().parts.version != id.parts.version → return null
  4. return actor

remove(id):
  1. actors_[id.index].store(nullptr, release)
  2. versions_[id.index].fetch_add(1, relaxed)
  3. free_indices_.enqueue(id.index)
```

### 6.3 Actor 邮箱 (Intrusive MPSC)

```cpp
// 多生产者入队 (任意 Worker 可调用)
bool Actor::push(ActorMessage* msg) {
    msg->next.store(nullptr, relaxed);
    auto* prev = mailbox_tail_.exchange(msg, acq_rel); // 原子交换
    prev->next.store(msg, release);                    // 发布
    return mailbox_tail_ == &mailbox_head_;            // 原为空 → true
}

// 单消费者出队 (仅绑定 Worker 可调用)
ActorMessage* Actor::pop_all() {
    auto* head = mailbox_head_.next.load(acquire);
    if (!head) return nullptr;
    mailbox_head_.next.store(nullptr, relaxed);
    return head;  // 单线程遍历链表处理
}
```

### 6.4 消息生命周期

```
                  分配                   处理                  归还
NetworkMessage:  PacketPool ──→ Handler ──→ NetworkMessagePool::release()
RpcMessage:      new        ──→ Handler ──→ delete this (finalize)
SceneMsg:        new        ──→ Handler ──→ delete this (finalize)
ActorDiedMsg:    new        ──→ Parent  ──→ delete this (finalize)
```

---

## 7. 协议层详解

### 7.1 协议帧格式

```
 [0x00-0x03]  [0x04-0x07]  [0x08...]
 ┌───────────┬─────────────┬────────────────┐
 │  Length    │   MsgID     │  Protobuf Body │
 │ (BigEndian)│ (BigEndian) │  (Length - 4B) │
 └───────────┴─────────────┴────────────────┘
```

### 7.2 Connection 拆包流程

```
rx_buffer_ 累积 TCP 字节流:

Step 1: rx_len_ < 4?
    └─ Yes → co_await 等待更多数据
    └─ No  → 读取 4B 帧头

Step 2: 帧头解析
    total_len = ntohl(*(uint32_t*)rx_buffer_)
    if total_len > K_MAX_PACKET_SIZE (10MB) → 断开连接

Step 3: rx_len_ < 4 + total_len?
    └─ Yes → co_await 等待完整数据包
    └─ No  → 从 PacketPool 分配 PooledPacket
              memcpy(data, rx_buffer_ + 4, total_len - 4)
              返回 PooledPacket (RAII, 自动归还)
```

---

## 8. 时间轮 (HierarchicalTimeWheel)

### 8.1 层级结构

```
Level 1 (tv1):  256 slots × 50ms   = 12.8s
Level 2 (tv2):   64 slots × 12.8s  = 13.65min
Level 3 (tv3):   64 slots × 13.65min = 14.56h
Level 4 (tv4):   64 slots × 14.56h  = 38.8 days
Level 5 (tv5):   64 slots × 38.8d   = 6.8 years
```

### 8.2 级联流程

```
tick():               # 每 50ms 调用一次
  current_tick_++
  idx = current_tick_ & TVR_MASK
  if idx == 0:        # 时间轮溢出 → 级联
    cascade(tv2)
    if (current_tick_ >> TVR_BITS) & TVN_MASK == 0:
      cascade(tv3)
      ...

cascade(list):
  for each node in list:
    重新计算 node->expires
    插入到正确的 tv1~tv5 槽
```

---

## 9. 性能关键路径总结

### 9.1 Hot Path (每数据包执行)

| 步骤 | 原子操作 | 内存分配 | 系统调用 |
|:---|:---|:---|:---|
| io_uring reap | 0 | 0 | 0 (SQPOLL) |
| Connection 拆包 | 0 | 0 (SBO) | 0 |
| PooledPacket 分配 | 0 (线程本地) | 0 | 0 |
| Actor push | 1 (exchange) | 0 | 0 |
| Worker 调度 | 0 (本地队列) | 0 | 0 |
| Handler 执行 | 0 (业务层无锁) | 0 (protobuf arena) | 0 |
| Outbox 聚合 | 0 | 0 (vector 预留) | 0 |
| io_uring submit | 0 | 0 | 0 (SQ 无竞争) |

### 9.2 Cold Path (偶发执行)

| 步骤 | 原因 | 频率 |
|:---|:---|:---|
| Work Steal | Worker 空闲 | 每隔几秒 |
| Outbox flush | iovec 满或连接即将休眠 | 每毫秒几次 |
| Work Stealing Queue resize | Actor 数量波动 | 每秒几次 |
| rx_buffer_ shrink | 空闲连接收缩缓冲 | 几秒一次 |
| 时间轮级联 | 定时器溢出更高一级 | 每 12.8s 或更长 |

---

## 10. 内存布局

### 10.1 Packet (Small Buffer Optimization)

```
stack_buf_[1024]        ← SBO: 小包直接栈上
heap_buf_ (if needed)   ← 大包 malloc
data_                   ← 指向当前活跃缓冲区 (stack 或 heap)
size_                   ← 已使用大小
capacity_               ← 容量 (1024 或 heap 大小)
```

### 10.2 IntrusiveListNode

```
TimerNode (占用 48 bytes):
  IntrusiveListNode: {next_, prev_} → 16 bytes
  expires: uint64_t                → 8 bytes
  cb: TimerCallback (std::function) → 32 bytes
  id: TimerId (uint64_t)           → 8 bytes
  padding                          → ~16 bytes
  ——————————————————————————————————
  Total: 80 bytes (含 padding)
```

### 10.3 ActorRegistry

```
actors_[65536]    → 65536 * 8 = 512KB      (atomic<Actor*>)
versions_[65536]  → 65536 * 4 = 256KB     (atomic<uint32_t>)
free_indices_     → ConcurrentQueue         (~数 KB)
——————————————————————————————————
Total: ~768KB + queue overhead
```

---

## 11. 与竞品对比

| 特性 | Aegis Engine | skynet | Noxi | Pomelo |
|:---|:---|:---|:---|:---|
| 语言 | C++20 | C+Lua | C++17 | JavaScript |
| IO 模型 | io_uring Proactor | epoll Reactor | epoll Reactor | libuv Reactor |
| 协程 | C++20 Coroutines | Lua Coroutines | ❌ | ❌ |
| 无锁 Actor | ✅ | ❌ (rwlock) | ❌ (mutex) | ❌ (mutex) |
| ObjectPool | ✅ | ❌ | ✅ (部分) | ❌ |
| Work Stealing | ✅ | ❌ | ❌ | ❌ |
| 零拷贝发送 | ✅ | ❌ | ❌ | ❌ |
| 内核级异步 | ✅ (io_uring) | ❌ (epoll) | ❌ (epoll) | ❌ (libuv) |
| 内存分配器 | jemalloc | tcmalloc | jemalloc | v8 GC |

---

## 12. 已知限制

| 限制 | 影响 | 未来方案 |
|:---|:---|:---|
| Linux 5.10+ only | 无法用于 Windows/macOS 服务器 | WSL2 兼容层或 io_uring 桥接 |
| 单进程架构 | 多进程部署需要额外设计 | gRPC SS 通信已规划 |
| 硬编码 Worker 数量 | 需重启调整 | 动态扩缩容 |
| 无持久化层 | 玩家数据纯内存 | 异步 DB (PostgreSQL + Redis) |
| 无热更新 | 业务逻辑变更需重编译 | Lua/ WASM 热更嵌入 |
| 无连接加密 | TCP 明文传输 | OpenSSL/TLS 封装 |
