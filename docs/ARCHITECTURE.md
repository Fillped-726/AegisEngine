# AegisEngine 架构文档

> 如果你只想读一篇文档了解这个项目，读这篇就够了。

---

## 设计哲学

1. **自研优先**：核心网络层和运行时全部自研，不依赖 libuv/Boost.Asio/CAF 等第三方库
2. **分层隔离**：`core/` (运行时) · `net/` (网络) · `game/` (业务) 三层独立演进
3. **零拷贝优先**：从 Packet 分配、跨 Worker 投递到 writev 发送，全程最小化数据复制
4. **可测试设计**：SyncManager 模板化解耦算法与发送逻辑，AOI Grid 纯函数式设计

---

## 第一层：网络基座 (core + net)

### 1.1 io_uring + C++20 协程

一切 I/O 都是 awaitable：

```cpp
// 读一个完整的网络包 —— 协程挂起直到数据就绪
auto pkt = co_await conn->read_packet();

// 发送 —— 入队后自动触发 writev 批量发送
conn->send(std::move(pkt));
```

每个 Worker 持有独立的 io_uring 实例。SQE 提交和 CQE 收割都在 Worker 线程内完成，无锁竞争。

**关键文件**:
- `include/aegis/net/socket.h` — AsyncRead/AsyncWrite/AsyncAccept awaiter
- `include/aegis/net/connection.h` — 连接状态机 (read_packet / send / flush)
- `include/aegis/core/worker.h` — Worker::run() 主循环 (IO + Actor + 时间轮)

### 1.2 零拷贝 Packet

```cpp
class Packet {
    char stack_buf_[1024];   // SBO: 90%+ 的游戏消息落在栈上
    char *heap_buf_;         // 大包回退堆分配
    // ...
};
```

PacketPool 预分配 100,000 个 Packet（约 100MB），池满后优雅降级为堆分配。

**OutboxBatcher** 将最多 64 个待发送包聚合成一次 `writev` 系统调用，实测在 Depth=1 场景下 P99 延迟仅 0.24ms。

**关键文件**: `include/aegis/net/packet.h` · `include/aegis/net/packetPool.h` · `include/aegis/net/outbox_batcher.h`

### 1.3 自定义二进制协议

```
[4B Magic "AEGS"][4B Body Length][4B SeqID][4B MsgID][Protobuf Body]
```

- 大端序（网络字节序）
- Magic 校验防止错位
- Connection::close() 在 Magic 校验失败时自动关闭连接

### 1.4 连接优雅关闭

```cpp
void Connection::close() {
    if (closed_.exchange(true)) return;  // 幂等
    rx_buffer_.clear();
    outbox_.buffer.clear();
    is_flushing_ = false;
    ::shutdown(fd, SHUT_WR);   // 发送 FIN
    socket_.close();            // 释放 fd
}
```

`closed_` 原子标志防止 `send_batch_coro` 和 `read_packet` 在关闭后继续提交 io_uring SQE。

### 1.5 背压保护

- 发送队列上限 1024 包，超出时丢弃最老的包
- io_uring_submit 失败时自动回退到 submit_and_wait，防止 SQ 环溢出

---

## 第二层：Actor 运行时 (core)

### 2.1 Actor 模型

每个游戏实体是一个 Actor（PlayerActor / SceneActor / NpcActor），拥有独立的无锁 MPSC 消息队列。

```cpp
class Actor {
    ActorMessage *head_;                    // Consumer: 仅绑定 Worker 读
    std::atomic<ActorMessage *> tail_;      // Producer: 任意线程 push
    // push: exchange tail, 链接旧尾到新节点
    // process_batch: 消费最多 100 条消息
};
```

**ActorID**: 64-bit (32-bit index + 32-bit version)，版本号防 ABA 问题，`raw == 0` 表示无效 ID。

**关键文件**: `include/aegis/core/actor.h` · `include/aegis/core/actor_registry.h`

### 2.2 协程 Task 三件套

| 类型 | initial_suspend | final_suspend | 用途 |
|------|----------------|---------------|------|
| `Task<T>` | suspend_always | 对称转移 | 可等待的异步任务 |
| `DetachedTask` | suspend_never | suspend_never | 入口函数 (fire-and-forget) |
| `MoveOnlyTask` | — | — | 类型擦除，跨 Worker 投递 |

**协程 RPC**:
```cpp
// 旧方式：阻塞 Worker 线程
auto res = future.get();

// 新方式：协程挂起，不阻塞
auto res = co_await RpcCall<AssignCampRes>(target_id, rpc_msg);
```

**关键文件**: `include/aegis/core/task.h` · `include/aegis/core/awaiter.h` · `include/aegis/game/rpc_awaiter.h`

### 2.3 Thread-per-Core Scheduler

```cpp
// Worker 主循环 (50ms tick)
while (is_running) {
    process_cross_core_messages();   // 其他 Worker 投递的 Actor
    process_local_tasks();           // 本 Worker Actor 的消息批处理
    time_wheel_.tick();              // 驱动定时器 (50ms)
    process_io();                    // io_uring 收割 + 提交
}
```

跨核通信：moodycamel 无锁队列 + eventfd 唤醒。

### 2.4 5 级分层时间轮

| 级别 | 精度 | 范围 |
|------|------|------|
| Level 1 | 50ms × 256 | 12.8s |
| Level 2 | 12.8s × 64 | 13.6min |
| Level 3 | 13.6min × 64 | 14.5h |
| Level 4 | 14.5h × 64 | 38.8d |
| Level 5 | 38.8d × 64 | 6.8y |

O(1) 添加/取消，侵入式链表 splice 实现零拷贝级联。

**关键文件**: `include/aegis/core/hierarchy_timer.h` · `include/aegis/common/intrusive_list.h`

---

## 第三层：游戏服务端架构 (game)

### 3.1 AOI 空间同步

9 宫格网格 + 偏移坐标系（支持负数范围）：

```
地图范围: [-1000, 1000] × [-1000, 1000]
Cell Size: 256
网格数: 8 × 8 = 64
9 宫格物理跨度: 768 × 768
```

脏标记驱动的增量状态同步，四阶段 Tick：
1. AOI Move + Enter/Leave 检测
2. 聚合每个接收者的更新批次
3. 批量发送（每个接收者一次序列化）
4. 清理脏标记

**关键文件**: `include/aegis/game/aoi_grid.h` · `include/aegis/game/sync_manager.h` · `include/aegis/game/scene_actor.h`

### 3.2 业务消息流

```
[TCP 到达] → io_uring CQ → Connection::read_packet()
  → Actor::push() MPSC 入队 → Worker drain
  → PlayerActor::handle_message()
  → Dispatcher::dispatch() → 业务 handler (协程)
```

**关键文件**: `services/gate/src/logic/handler_loader.cpp`

### 3.3 游戏业务组件

| 组件 | 职责 |
|------|------|
| PlayerActor | 玩家实体，连接管理，脏标记序列化 |
| SceneActor | 场景管理，AOI 网格，Tick 驱动，技能处理 |
| NpcActor | 怪物实体，行为树驱动的 AI |
| RoomManager | 营地创建/加入，RPC 协程 |
| GameApp | 游戏 Bootstrap，主城场景创建 |

---

## 基准测试

**环境**: WSL2, 8 vCPU, 16GB RAM, taskset 4 核服务端 + 4 核客户端

### Depth=1 (请求-响应延迟)

| Payload | 1 连接 | 256 连接 |
|---------|--------|---------|
| 32B | 8,116 qps / P99 0.24ms | 303,779 qps / P99 2.80ms |
| 128B | 8,929 qps / P99 0.21ms | 297,613 qps / P99 2.69ms |
| 512B | 7,816 qps / P99 0.24ms | 268,941 qps / P99 2.94ms |
| 1024B | 7,337 qps / P99 0.28ms | 273,003 qps / P99 3.08ms |
| 4096B | 5,656 qps / P99 0.33ms | 100,236 qps / P99 7.59ms |

### Depth=64 (流水线饱和吞吐)

| Payload | 16 连接峰值 | 延迟 |
|---------|------------|------|
| 32B | 2,436,022 qps | P50 0.36ms / P99 1.25ms |
| 128B | — | — |
| 512B | — | — |
| 1024B | — | — |
| 4096B | — | — |

---

## 性能设计要点

- **SBO 命中率**: 90%+ 的游戏消息落在栈上，只有 AOI 广播包走堆分配
- **缓存行对齐**: Actor 的关键字段 64 字节对齐，防 false sharing
- **批量发送**: writev 一次系统调用聚合 64 个包
- **零拷贝广播**: ForwardPacketMsg 共享 `shared_ptr<string>`，多个收件人共享同一份序列化数据
- **ActorID 防 ABA**: 32-bit version 字段，每次从池中复用 +1，get() 时校验版本
