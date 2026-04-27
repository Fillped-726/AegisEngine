# 网络层 — Socket / Connection / Packet / Dispatcher

> 对应目录: `include/aegis/net/` · `src/net/`

---

## 1. 总体架构

```
           ┌──────────┐
           │ Acceptor │  ← 监听端口，接受连接
           └────┬─────┘
                │ Socket (RAII fd 包装)
          ┌─────┴──────┐
          │ Connection │  ← 每个 TCP 连接一个实例，shared_ptr 管理
          │            │     负责拆包 / 组包 / 发送
          └─────┬──────┘
     ┌──────────┼──────────┐
     ▼          ▼          ▼
  read_packet()  send()   flush()
  (协程读取)    (入队)   (writev 聚合发送)
```

---

## 2. Socket (`socket.h`)

RAII 包装 POSIX fd，提供 io_uring 异步操作 awaitables。

```cpp
class Socket {
    UniqueFd fd_;  // RAII fd

    // io_uring awaitables（内嵌类）
    struct AsyncRead { /* co_await socket.recv(buf, len) */ };
    struct AsyncWrite { /* co_await socket.send(buf, len) */ };
    struct AsyncReadV { /* co_await socket.recv(iovec*, count) */ };
    struct AsyncWriteV { /* co_await socket.send(iovec*, count) */ };
    struct AsyncAccept { /* co_await acceptor.accept() → Socket */ };
};
```

**io_uring 操作码**: 所有 I/O 操作通过提交 SQE 实现，内核完成后再恢复协程。

---

## 3. Acceptor (`acceptor.h`)

```cpp
class Acceptor {
    Socket listen_socket_;   // 监听 Socket
    Task<Socket> accept();  // 协程接受连接 → 返回 Socket
};
```

- 在 GateServer::accept_loop() 中运行（绑定 Worker 0）
- `co_await acceptor.accept()` → 返回新连接的 Socket

---

## 4. Connection (`connection.h`)

**每个 TCP 连接一个 Connection 实例**，使用 `shared_ptr` 管理生命周期（因为协程可能异步持有引用）。

```cpp
class Connection : public std::enable_shared_from_this<Connection> {
    Socket socket_;
    Outbox outbox_;         // 待发送包队列 (vector<PooledPacket>)
    OutboxBatcher batcher_;  // writev 聚合

    // 接收缓冲
    std::vector<char> rx_buffer_;
    size_t rx_len_ = 0;

    // 核心 API
    Task<PooledPacket> read_packet();    // 协程读取一个完整包
    void send(PooledPacket packet);      // 入队发送
    void flush();                        // 手动触发发送
};
```

### 4.1 拆包协议 (read_packet)

**Wire Format (新版，带 SeqID)**:
```
[4B Length BigEndian][4B SeqID BigEndian][4B MsgID BigEndian][Protobuf Body]
 Length = 8 (SeqID+MsgID) + BodySize
```

> 注意：对比旧版，客户端-服务端协议现在在报文头中插入了 `SeqID`（4 字节）。
> 实际的 Packet 内部格式是 `[4B SeqID][4B MsgID][Body]`。
> 在 OutboxBatcher 发送时，还会在外层加上帧长度头 `[4B Length]`。

**Connection::read_packet() 拆包流程**:
```
rx_buffer_ 累积 TCP 字节流:

Step 1: rx_len_ < 4?
    └─ Yes → co_await 等待更多数据
    └─ No  → 读取 4B 帧头

Step 2: 帧头解析 (帧层)
    total_len = ntohl(*(uint32_t*)rx_buffer_)
    if total_len > K_MAX_PACKET_SIZE (10MB) → 断开连接

Step 3: rx_len_ < 4 + total_len?
    └─ Yes → co_await 等待完整数据包
    └─ No  → 从 PacketPool 分配 PooledPacket
              memcpy(data, rx_buffer_ + 4, total_len)
              返回 PooledPacket (RAII 自动归还)
```

### 4.2 发送流程 (send + flush)

```cpp
void Connection::send(PooledPacket packet) {
    outbox_.buffer.push_back(std::move(packet));
    if (!is_flushing_) flush();
}
```

**flush → send_batch_coro**:
1. `batcher.prepare_batch(outbox_.buffer)` — 构建 iovec 数组
2. `co_await socket_.writev(batcher.iov_data(), batcher.iov_count())` — 单次 writev
3. `batcher.advance(bytes_written)` — 处理部分写入，未完成的包重回 outbox

### 4.3 收发缓冲区常量

| 常量 | 值 | 说明 |
|------|-----|------|
| K_INITIAL_RX_SIZE | 4096 | 初始接收缓冲区 |
| K_SHRINK_THRESHOLD | 1MB | 空闲时收缩阈值 |
| K_MAX_PACKET_SIZE | 10MB | 单包最大长度，超限断开 |

---

## 5. Packet (`packet.h`)

SBO (Small Buffer Optimization) 网络包，**1024B 栈内缓冲**，大包回退堆分配。

### 5.1 内存布局

```
Packet (约 1056 bytes)
┌─────────────────────────────────────┐
│ stack_buf_[1024]  (SBO 热数据区)    │
├─────────────────────────────────────┤
│ heap_buf_  (8B) → 大包时指向堆      │
│ data_      (8B) → 指向活跃数据位置   │
│ size_      (8B)                      │
│ capacity_  (8B)                      │
│ seq_id_    (4B)                      │
└─────────────────────────────────────┘
```

大部分游戏消息 < 1KB，SBO 覆盖全部典型消息。只有 AOI 广播批量包会走堆分配。

### 5.2 核心 API

| 方法 | 说明 |
|------|------|
| `pack_into(msg_id, seq_id, protobuf_msg)` | 序列化：写 SeqID + MsgID + Body 到内部缓冲 |
| `parse<T>(protobuf&)` | 反序列化：从 Body 区域解析 protobuf |
| `alloc(size)` | 分配空间（自动选择 SBO/堆） |
| `msg_id()` | 读 BigEndian MsgID |
| `seq_id()` | 读/写 BigEndian SeqID |

### 5.3 协议常量

```cpp
// 帧层 (OutboxBatcher/read_packet 处理)
constexpr size_t kFrameLengthSize = 4;    // 帧长度头
constexpr size_t kFrameHeaderSize = 4;    // 保留 (用于 Magic? 实际已废弃)

// 包层 (Packet 内部)
constexpr size_t kPacketSeqIdSize = 4;    // SeqID 字节数
constexpr size_t kPacketMsgIdSize = 4;    // MsgID 字节数
constexpr size_t kPacketMsgHeader  = 8;   // SeqID + MsgID

// 魔数 (保留)
constexpr uint32_t kAegisMagic = 0x41454753; // 'AEGS'
```

---

## 6. PacketPool (`packetPool.h`)

```cpp
using PacketPool = ObjectPool<Packet, 100000, 128>;
using PooledPacket = PacketPool::Ptr;  // RAII 自动释放
```

- 最多预分配 100000 个 Packet，约 100MB
- 池满后回退堆分配（优雅降级）
- BatchSize=128 平衡 TLS 和全局池访问

---

## 7. OutboxBatcher (`outbox_batcher.h`)

零拷贝 writev 聚合器，**64 个包一次系统调用**。

```cpp
class OutboxBatcher {
    size_t prepare_batch(std::vector<PooledPacket>& queue);  // 构建 iovec
    size_t advance(size_t bytes_written);                     // 处理部分写入
    struct iovec* iov_data();                                  // iovec 数组
    int iov_count();                                           // iovec 数量
    bool is_empty();

    static constexpr size_t BATCH_LIMIT = 64;
};
```

**工作原理**:
1. `prepare_batch()` 从队列前端取最多 64 个包
2. 每个包的 `[4B length][data]` 写入 iovec 数组（length 由 batcher 本地内存提供，不修改 Packet 内容）
3. `writev(fd, iov_data(), iov_count())` — 单次系统调用发送所有包
4. `advance(bytes_written)` 处理部分写入：已发送的包出队，未完成的包保留

---

## 8. Dispatcher (`dispatcher.h`)

```cpp
class Dispatcher {
    using NetHandler = std::function<Task<void>(Actor*, const char*, size_t)>;
    using RpcHandler = std::function<Task<void>(Actor*, const void*)>;

    // 注册 Net 处理器（protobuf 反序列化 + 调用）
    template<ProtobufMessage ProtoMsg, typename Func>
    void register_handler(uint32_t msg_id, Func&& func);

    // 注册 RPC 处理器（零开销 static_cast）
    template<typename RpcMsgType, typename Func>
    void register_rpc(uint32_t msg_id, Func&& func);

    // 分发路径
    Task<void> dispatch(Actor* actor, const Packet& pkt);        // Net
    Task<void> dispatch_rpc(Actor* actor, uint32_t, const void*); // RPC
};
```

**两种分发模式**:

| 模式 | 注册 API | 调用 API | 开销 |
|------|---------|---------|------|
| **Net** | `register_handler<Proto>(msg_id, handler)` | `dispatch(actor, packet)` | protobuf 反序列化一次 |
| **RPC** | `register_rpc<RpcMsg>(msg_id, handler)` | `dispatch_rpc(actor, msg_id, ptr)` | 零拷贝 static_cast |

双路径设计：Net 用于客户端网络消息，RPC 用于服务端 Actor 间消息。

---

## 9. PacketBuilder (`packet_builder.h`)

```cpp
struct EntityViewInfo { uint64_t uid; float x; float y; uint32_t entity_type; };

class PacketBuilder {  // 纯静态工厂
    static shared_ptr<string> BuildEnterView(const EntityViewInfo& entity);
    static shared_ptr<string> BuildEnterView(span<const EntityViewInfo> entities);  // 批
    static shared_ptr<string> BuildLeaveView(uint64_t uid);
    static shared_ptr<string> BuildLeaveView(span<const uint64_t> uids);            // 批
};
```

专用于 AOI Enter/Leave 视野消息的 protobuf 序列化，返回 `shared_ptr<string>` 用于广播共享。

---

## 10. 消息流转全路径

```
[TCP 数据到达]
    │
    ▼
io_uring CQ (内核填充完成事件)
    │
    ▼
Connection::read_packet() 协程恢复
    │
    ▼ 拆包完成 → PooledPacket
GateServer::handle_session() 循环体
    │
    ▼ dispatch_to_actor()
Actor::push(msg) — MPSC 无锁入队
    │
    ▼ Worker drain → handle_message()
PlayerActor::handle_message(MSG_TYPE_NETWORK)
    │
    ▼ Dispatcher::dispatch(actor, packet)
Dispatcher::net_handlers_[msg_id](actor, body_data, body_len)
    │
    ▼ protobuf 反序列化 → 业务 handler
handler(actor, parsed_proto)
```

**路径清晰标注**:
| 阶段 | 文件 | 关键函数 |
|------|------|---------|
| io_uring CQ | `connection.cpp` | `Connection::read_packet()` |
| 消息投递 | `gate_server.cpp` | `GateServer::dispatch_to_actor()` |
| Actor 入队 | `actor.h` | `Actor::push()` |
| Actor 业务处理 | `actor.cpp` | `Actor::process_batch()` → `handle_message()` |
| Handler 映射 | `dispatcher.h` | `Dispatcher::dispatch()` |
| 业务 Handler | `handler_loader.cpp` | 注册的 lambda |
