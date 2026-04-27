# AegisEngine 面试准备文档

> 难点、亮点、设计决策深度解析，用于面试中的技术深挖环节。
> **已更新**：补充 RPC 协程、营地系统、消息重构、GateServer 瘦身等内容。

---

## 一、架构亮点总览

| 维度 | 亮点 |
|------|------|
| **IO 模型** | 基于 io_uring + C++20 协程的全异步 I/O，零系统调用上下文切换 |
| **Actor 模型** | 无锁 Actor 注册中心 + 64 位 Handle（含版本号防 ABA） |
| **内存管理** | 三层对象池架构（TLS 缓存 + 全局池），侵入式链表，SBO 小包优化 |
| **Actor 间通信** | 协程 RPC —— `co_await RpcCall<ResT>()` 替代阻塞 `future.get()` |
| **状态同步** | 脏标记驱动 + AOI 网格（9宫格）+ 批量打包共享 Buffer |
| **NPC AI** | BehaviorTree.CPP v4 集成 + AOI 驱动的 AI 唤醒/休眠 |
| **定时器** | 分层时间轮（5级256+64×4），O(1) 所有操作，零拷贝级联 |
| **模块解耦** | GameApp 抽取、目录拆分（core/ vs game/）、GateServer 瘦身 60% |
| **营地系统** | 动态营地创建/加入/查询 + 缓存 + 人数自动上报 |

---

## 二、高频面试题

### Q1: 为什么选择 io_uring？

**传统方案对比：**
- epoll: 每次事件循环需要 `epoll_wait` 获取就绪事件，然后发起 read/write（两次 syscall）
- io_uring: 通过 SQ/CQ 两个共享环形缓冲区与内核通信，**同一次系统调用可以提交多个 IO 请求并收割完成结果**
- **关键差异**：io_uring 对于大量小包场景（游戏服务器典型负载），系统调用开销降低 50%+

**为什么会想到 io_uring：**
这是关键问题。AegisEngine 的设计师是从 **「减少系统调用次数」** 这个根本问题出发的。游戏服务器的特点是：每秒数万个 TCP 连接，每条连接多次小包读写。如果用 epoll + 非阻塞 IO，每次读写都是一次系统调用。io_uring 的提交/完成队列模式完美解决这个问题。

### Q2: 为什么用 C++20 协程，而不是传统回调或状态机？

**协程 vs 回调的代码复杂度对比：**

```cpp
// 回调版本（伪代码）
void on_readable(conn, callback) {
    auto* buf = allocate();
    async_read(conn, buf, sizeof(buf), [=](int nread) {
        if (nread == -1) { free(buf); callback(err); return; }
        process(buf, nread);
        free(buf);
        callback(ok);
    });
}
// 注意：错误处理、内存释放、控制流交织——每一层回调都翻倍复杂度
```

```cpp
// 协程版本（AegisEngine 实际代码）
Task<PooledPacket> Connection::read_packet() {
    // 协程内的每一步都可以 co_await，控制流是线性的
    auto pkt = PacketPool::instance().acquire();
    co_await socket_.recv(pkt->mutable_data(), K_HEADER_SIZE);
    // ... 线性展开，异常安全通过 RAII 保证
    co_return pkt;
}
```

**核心收益：**
1. 线性代码 = 可读性高
2. RAII 天然异常安全（不再有回调式中漏掉 free 的问题）
3. 协程链自动管理生命周期，无需手工 ref-count

### Q3: ActorID 为什么设计成 64 位（32位下标+32位版本号）？

**这是为了解决「悬空指针」和「ABA 问题」**，同时也是游戏服务器面试中的经典话题。

```cpp
union ActorID {
    struct { uint32_t index; uint32_t version; };
    uint64_t raw;  // 可一次性 64 位比较、哈希、网络传输
};
```

场景：玩家 A 在 index=42，后来断开连接。index=42 被回收。版本号+1。新的玩家 B 被分配到 index=42（但版本号不同）。客户端发来一条消息引用旧玩家的 ActorID（index=42, old_version）。ActorRegistry::get() 发现版本不匹配，返回 nullptr，安全拒绝。

**对比方案：**
- 裸指针：被回收后悬挂，100% 崩溃
- shared_ptr：循环引用 + 性能开销
- 64 位 Handle：O(1) 查找，版本号验证，deref 只是 atomic load

### Q4: 对象池为什么设计成 TLS + 全局池的两层架构？

**ObjectPool<T, MaxSize, BatchSize>：**

```
Each thread:
  ┌──────────────┐
  │ TLS Batched  │  ← 无锁，只有线程本地操作
  │ Queue (Batch)│
  └──────┬───────┘
         │ batch acquire/release
  ┌──────┴───────┐
  │ Global Pool  │  ← moodycamel::ConcurrentQueue 实现
  │ (MaxSize)    │     多生产者/消费者，无锁
  └──────────────┘
```

- **TLS 层**：线程本地 batch 操作，热点路径无任何原子操作
- **全局层**：当 TLS batch 耗尽时从全局取一批（BatchSize 个）；回收同理
- **为什么 BatchSize 默认 128**：实测表明 128 是在缓存行友好和减少全局访问之间的最佳平衡点

**PacketPool 特化（ObjectPool<Packet, 100000, 128>）：**
Packet 是 1056 字节（SBO 1024B + 元数据）。100000 个包预分配约 100MB。这是有上限的——如果连接数远超预期，池满后会回退到堆分配（优雅降级，非 crash）。

### Q5: OutboxBatcher 的设计动机？

**零拷贝 + writev 聚合发送**，避免每个包都做一次系统调用。

```cpp
// 批量发送 64 个包，只调用一次 writev
size_t count = batcher.prepare_batch(queue);  // 构建 iovec 数组
int ret = ::writev(fd, batcher.iov_data(), batcher.iov_count());
size_t completed = batcher.advance(ret);       // 处理部分写入
```

**关键点：** 每个包的 4 字节长度头写入 `header_cache_` 向量，而不是每个包的 data() 前面。这是因为 Packet 的 data() 指向的是包体[seq_id+msg_id+body]，封装层的长度由 OutboxBatcher 在发送时才添加——这是一种解耦设计。

### Q6: 分层时间轮为什么好过 std::priority_queue？

```
传统定时器：std::priority_queue
  - add_timer: O(log N)
  - tick: O(log N) 弹出超时
  - cancel: O(N) 查找（除非额外 map）

分层时间轮（5级）：
  - add_timer: O(1)
  - tick: O(1) 平均（偶尔级联 O(N) 但均摊 O(1)）
  - cancel: O(1) via timer_map_
  - 零拷贝级联：用 IntrusiveList 拼接，不分配内存
```

**为什么游戏服务器需要 O(1) 的定时器？**
每个 AOI 广播、技能 CD、状态持续、Buff 计时都可能触发定时器。如果 N=10000 并发定时器，priority_queue 的 add 是 O(log 10000) ≈ 14 次比较。时间轮是 O(1)。

**TICK_MS=50 的设计考量：**
游戏逻辑不需要纳秒级精确。50ms = 20fps 的逻辑更新周期，对齐大多数游戏的状态同步帧率。

### Q7: SyncManager 为什么是模板化的 Tick？

```cpp
template <typename EnterLeaveFunc, typename SendBatchFunc>
void Tick(AOIGrid &aoi, EnterLeaveFunc &&onEnterLeave, SendBatchFunc &&onSendBatch);
```

**设计动机：** SyncManager 只负责「计算谁该看到谁」、「聚合哪些数据」，不负责「怎么发送」。SceneActor 通过两个回调注入真正的发送逻辑。这是策略模式 + 编译期多态的体现：
- SyncManager 保持纯算法（可测试）
- SceneActor 持有 PlayerActor 和 Connection 引用（可发送）
- 编译期通过模板消除虚函数调用开销（热路径优化）

### Q8: NpcActor 的 AI 如何与 AOI 联动？

```cpp
class NpcActor {
    BT::Tree tree_;
    bool is_ai_active_ = false;
};
```

- 当玩家进入 NPC 的 AOI 范围 → SceneActor 将 NPC 标记为 `is_ai_active_ = true`
- 当玩家离开 → 标记为 `is_ai_active_ = false`，停止 Tick AI
- 下一帧 SceneActor::OnTick() 只对活跃 NPC 调用 NpcActor::OnTick()
- **收益**：大世界 90%+ 的 NPC 可能与玩家无交互，AI 不执行，CPU 省 90%

### Q9: 跨线程消息如何保证安全？

```
Worker A                    Worker B
  │                           │
  │─enqueue_message(msg)──►   │
  │   (lock-free push)        │
  │                           │─drain messages (single-thread inside Actor)
```

- `Actor::push()` 使用 lock-free MPSC入队（基于 atomic exchange）
- 目标 Actor 在哪个 Worker 上创建，消息就投递到哪个 Worker
- 接收侧：Worker 的 run loop 每个迭代都会 drain 自己管辖 Actor 的消息队列
- **Actor 内部是单线程模型**——这是分布式 Actor 系统的核心保证

### Q10: 协程中的协程链（Task chain）怎么执行的？

```cpp
Task<void> handler(Actor* actor, const LoginReq& req) {
    auto result = co_await db_query(req.user_id);  // 挂起，让出线程
    auto response = build_response(result);
    co_await actor->send_packet(MSG_LOGIN_RSP, response);
}
```

**执行路径：**
1. `handler()` 在调用者线程上被 `co_await` 启动
2. 遇到 `co_await db_query()` → 提交 io_uring SQE → 挂起
3. Worker 的 io_uring 完成处理循环：CQE 到达 → 找到挂起的协程句柄 → `resume()`
4. 继续执行直到下一个 `co_await`... 或 `co_return`
5. `final_suspend` → FinalAwaiter 对称转移 → 恢复等待者（如果是 DetachedTask，自动销毁）

### Q11: RPC 为什么从同步阻塞改成协程异步？（新增）

**旧方案的问题**：
```cpp
auto future = rpc_msg->promise.get_future();
dispatch_msg(target, rpc_msg);
auto res = future.get();     // ⚠️ 阻塞当前 Worker 线程！
```

阻塞 Worker 线程 → 该 Worker 上的所有 Actor 都无法处理消息 → 实质上的串行化。

**新方案**：
```cpp
auto res = co_await RpcCall<AssignCampRes>(target_id, rpc_msg);
```

**实现要点**：
- `RpcManager`（thread_local 单例）：管理每个 Worker 的挂起 RPC
- `RpcAwaiter`（自定义 awaitable）：`await_suspend` 注册挂起 + 超时定时器
- `RpcMessage::Reply()` 智能双模式：自动选择 promise 或消息投递
- RPC ID 格式：`[16-bit WorkerID | 48-bit Counter]`，全局限唯一

**面试中可以强调**：这不是简单的语法糖，而是从**阻塞同步到非阻塞异步的架构级迁移**，涉及 `RpcMessage` 的 `Reply()` 双模式兼容设计、超时兜底、以及发起者 Actor 存活验证。

### Q12: 近期做了哪些架构重构？（新增）

1. **目录拆分** (`3276af8`): `game/` 从 `core/` 中分离，GateServer 不再直接依赖游戏逻辑头文件
2. **Message 系统重构** (`80fe8c2`): 单文件变 6 文件，`type_id` 从 `uint8_t` 升级到 `uint16_t`
3. **GateServer 瘦身** (`f5dfc1c`): 业务逻辑迁移到 `GameApp` + `handler_loader`
4. **RPC 协程化** (`486c3ea`): 同步 `future.get()` → 异步 `co_await RpcCall`

**面试中强调方法**："我在重构时遵循的是**增量替换**而不是一次性大改。比如消息系统的重构，我先保持旧接口兼容（BasicMessage 模板写法不变），只改内部实现，所有现有 handler 代码零改动。"

---

## 三、设计决策反面问题

### 如果让你重做这个项目，你会改什么？

**诚实回答建议：**

1. **io_uring 绑定 Linux**：虽然性能好，但放弃 Windows/macOS 是硬伤。如果有跨平台需求，应该抽象 IO 层（epoll/kqueue/IOCP + IORING 后端）。

2. **协程内存分配**：每个 `co_await` 的 Task 对象都在堆上分配（`operator new` 在 promise_type 中）。如果 Tick 频率高，这会触发大量小对象分配。解决方案：定制 coroutine frame allocator 或使用 ObjectPool 分配 Task 内存。

3. **Actor 间消息序列化**：当前跨线程用 `ForwardPacketMsg` 传递共享指针（`shared_ptr<string>`），减少了拷贝但引入了引用计数开销。如果做零拷贝优化，可以引入「拥有权转移」（类似 unique_ptr）。

4. **行为树集成深度**：目前 NpcActor 只是在自己的 Tick 里跑 BT，没有与行为树的事件驱动模式做深度联动（如"当受伤时触发逃跑树"）。可以引入 BT 的事件节点。

5. **缺少灰度/热更新**：RoomManager 的创建逻辑是静态的，没有热更新配置的能力。生产环境需要配置中心（如 etcd/consul）动态下发房间参数。

### 如果 QPS 暴涨 10 倍，系统哪里会先扛不住？

**瓶颈分析路径：**
1. `ActorRegistry::actors_` 数组：当前的 `std::atomic<Actor*>` release/acquire 语义在 x86 上是免费的。先不在这里。
2. `PacketPool` 满 100000：超过后回退到 `new/delete`，GC 压力增大
3. **OutboxBatcher**：`writev` 单次调用最多 IOV_MAX（Linux 默认 1024），但 BATCH_LIMIT=64 限制了单批 64 个包。如果单个连接发送量暴增，需要在多个 writev 调用间轮转。
4. **真正的瓶颈可能是**：每个 `send_buffer()` 都走 `PacketPool::acquire()`，高并发下 TLS batch 耗尽 → 全局 pool CAS 竞争。可以通过增大 BatchSize 缓解。

---

## 四、关键技术细节（展示深度）

### 内存布局：Packet SBO

```
Packet (1056 bytes total)
┌──────────────────────────────────────────────┐
│ stack_buf_[1024]  (SBO data)                 │
├──────────────────────────────────────────────┤
│ heap_buf_  (8 bytes)  → 如果 >1024B 则指向堆 │
│ data_      (8 bytes)  → 指向活跃数据位置     │
│ size_      (8 bytes)                             │
│ capacity_  (8 bytes)                             │
└──────────────────────────────────────────────┘
```

大部分游戏消息 < 1KB，SBO 覆盖全部。只有 AOI 广播批量包会走堆分配。

### 侵入式链表 vs std::list

```
std::list<TimerNode>:
  T (24B) + prev/next (16B) = 40B 每个节点，分配在堆上

IntrusiveList<TimerNode>:
  TimerNode (24B) + IntrusiveListNode (16B 内嵌) = 40B 总大小
  但 TimerNode 来自 ObjectPool！所以 40B 都在预分配的连续内存中
  没有额外的堆分配，CPU 缓存友好
```

为什么时间轮级联要「零拷贝」：当 tick 从 tv1[0] 溢出到 tv2 时，传统做法是遍历并重新插入。AegisEngine 用 IntrusiveList 的 splice 操作（`O(1)` 移动整个链表）。

### Protobuf 序列化优化：共享 Buffer

```cpp
// SyncManager 中：
auto shared_buf = std::make_shared<std::string>(batch.SerializeAsString());
// 多个 PlayerActor 共享同一个 serialized buffer！
```

这是关键优化：假设一次 AOI Tick 中有 10 个移动的玩家，30 个观众。如果每个观众单独序列化 → 30 次序列化 + 30 次网络发送。共享 buffer → 只序列化一次（每个 receiver 一个 batch），随后通过 shared_ptr 传递。虽然 C++ 的 shared_ptr 有原子引用计数开销，但相比 protobuf 序列化（内存分配+拷贝），这是值得的。

### 新协议头格式 (v2, 含 SeqID)

```
[4B Length BigEndian][4B SeqID BigEndian][4B MsgID BigEndian][Protobuf Body]
 Length = 8 (SeqID+MsgID) + BodySize
```

SeqID 格式：`[8-bit WorkerID | 24-bit Counter]`，每个 Worker 独立生成，用于请求-响应匹配。
引入 SeqID 后，客户端可以在多个未完成的请求中唯一标识哪个响应对应哪个请求。

### RPC 超时机制

在 `await_suspend` 中，通过 `HierarchicalTimeWheel` 注册一个超时定时器。如果 `RpcResponseMsg` 未在 5 秒（默认）内返回，定时器触发 `RpcManager::on_timeout(rpc_id)`，将 `result_` 置为 nullptr 并恢复协程。`await_resume()` 检测到 `result_ == nullptr` 后抛出 `std::runtime_error("RPC timed out")`。

---

## 五、面试回答模板

### "请介绍一下你的项目"

> AegisEngine 是一个基于 C++20 协程 + io_uring 的 MMO 游戏服务器引擎。它使用 Actor 模型组织业务逻辑——每个玩家、场景、NPC 都是一个 Actor，有自己的消息队列和 worker 线程绑定。IO 层完全异步，通过 io_uring 提交/完成队列实现零系统调用开销。网络协议使用 protobuf 序列化，加上一个大端序帧头（含 SeqID 支持请求-响应匹配）。对象池贯穿整个项目——Packet、Actor、TimerNode 都从池中分配。状态同步方面，使用基于 AOI 网格的脏标记驱动方案，批量打包、共享 buffer、零拷贝广播。近期做了几个大的重构：RPC 从同步阻塞改成了协程异步、GameApp 抽取和 GateServer 瘦身、消息系统按职责拆分为 6 个文件。

### "最困难的技术挑战是什么？"

> 挑战最大的部分是 C++20 协程与 io_uring 的集成。需要实现自定义的 promise_type，让 `co_await socket.recv()` 能够提交 io_uring SQE 并在 CQE 到达时恢复。还要支持协程链——`co_await` 一个返回 Task\<T\> 的函数时，被等待的协程完成后自动恢复调用者。同时要解决：协程内的异常传播、协程生命周期管理、以及 DetachedTask 的 fire-and-forget 语义。这些在 C++20 协程标准库刚出时几乎没有现成方案，全部是自定义实现。

> 另一个挑战是 RPC 协程化重构。原来的 RPC 通过 `std::promise` + `future.get()` 实现，但它**阻塞 Worker 线程**——这意味着该 Worker 上的所有玩家消息都卡住。改为 `co_await RpcCall<>` 后，非阻塞挂起让 Worker 可以在这段时间处理其他 Actor。这个重构需要兼容旧模式（通过 `RpcReplyMode` 双模式派发），同时保证超时异常安全和发起者 Actor 销毁时的安全性。
