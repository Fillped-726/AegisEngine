# Aegis Engine: Network Layer Technical Documentation

> **Status:** Engine Core / I/O Hub
> 
> **Language:** C++20 (Coroutines + Concepts)
> 
> **Key Attributes:** io_uring, Zero-Copy, Zero-Allocation, Proactor

## 1. 模块全景图 (The Big Picture)

### 1.1 定位与目标

**Network Layer** 是 Aegis 引擎的 **全异步通信底座**。

- **一句话定位：** 基于 Linux `io_uring` 和 C++20 协程构建的 **全异步、零分配、零拷贝** 高并发传输层。
    
- **职责：** 承接 OS 内核 IO 事件，向上传递强类型消息给 Actor 系统。
    

### 1.2 职责边界

|   |   |
|---|---|
|**负责 (In-Scope)**|**不负责 (Out-of-Scope)**|
|**生命周期** (TCP Connection Accept/Close)|业务逻辑状态维护|
|**Framing** (粘包/拆包处理)|Actor 调度执行|
|**IO 模型** (io_uring 提交与收割)|协议结构体定义 (Protobuf)|
|**内存视图** (iovec 构建, Scatter/Gather)||
|**路由分发** (基于 MsgID 的强类型分发)||

### 1.3 依赖关系

- **向下依赖:** Linux Kernel (`io_uring`, `Socket API`), C++20 Coroutines, **Common Module** (ObjectPool, SpinLock).
    
- **向上支撑:** Actor System (提供透明的消息投递接口).
    

## 2. 架构与类图 (Architecture)

网络层被划分为 **接入组**、**数据组**、**路由组** 三大块。

### 2.1 核心组件

1. **Acceptor & Socket:** 负责系统级 Socket 句柄管理与 io_uring SQE 的提交。
    
2. **Connection:** 核心容器，管理读写缓冲、协程状态机、发送队列 (Outbox)。
    
3. **OutboxBatcher:** 发送优化器，将多个逻辑包聚合成物理上的 `iovec` 数组 (Scatter/Gather)。
    
4. **PacketPool & Packet:** 结合 Common 层的内存池，实现 SBO (Small Buffer Optimization) 和零分配。
    
5. **Dispatcher:** 基于 C++20 Concepts 的类型安全路由，通过 Lambda 闭包擦除类型。
    

### 2.2 类关系图 (Class Diagram)

```
classDiagram
    %% 接入与连接组 (Transport Group)
    class Acceptor {
        +accept() Task~Socket~
        -optimize_client_socket()
    }
    class Socket {
        +recv() AsyncRead
        +send(iovs) AsyncWriteV
    }
    class Connection {
        -rx_buffer_
        -outbox_: Outbox
        +read_packet() Task~PooledPacket~
        +send(PooledPacket)
        +flush()
    }
    class OutboxBatcher {
        -iovecs_
        +prepare_batch()
        +advance()
    }

    %% 数据与内存组 (Data & Memory Group)
    class PacketPool {
        +acquire() PooledPacket
    }
    class Packet {
        -stack_buf_[1024]
        +alloc()
        +parse()
        +pack_into()
    }

    %% 路由分发组 (Routing Group)
    class Dispatcher {
        -net_handlers_
        +register_handler()
        +dispatch() Task~void~
    }

    %% 拓扑关系
    Acceptor ..> Socket : 1. Spawns
    Connection *-- Socket : 2. Wraps
    Connection *-- OutboxBatcher : 3. Uses for Send
    Connection ..> PacketPool : 4. Acquires memory
    PacketPool ..> Packet : 5. Pools
    Connection ..> Dispatcher : 6. Routes via
```

#### 3. 核心交互流程 (Key Workflows)

**流程 A：入站拆包与分发 (The Inbound Workflow)**

展示 C++20 协程如何优雅地“压平”原本极其复杂的粘包处理状态机。

代码段

```
sequenceDiagram
    participant OS as io_uring (Kernel)
    participant Conn as Connection
    participant Pool as PacketPool
    participant Disp as Dispatcher
    participant Actor as Business Actor

    Note over Conn, OS: 协程挂起，等待数据
    Conn->>OS: co_await socket_.recv(Header)
    OS-->>Conn: Resume (CQE Ready)
    Conn->>Conn: Parse Length (N)
    Conn->>Pool: acquire()
    Pool-->>Conn: PooledPacket (Free memory)
    Conn->>Conn: ensure_rx_capacity(N)
    
    Note over Conn, OS: 再次挂起，等待完整 Body
    Conn->>OS: co_await socket_.recv(Body)
    OS-->>Conn: Resume (CQE Ready)
    
    Conn->>Conn: memcpy to Packet
    Conn->>Disp: dispatch(actor, packet)
    Disp->>Disp: Hash Route (MsgID)
    Disp->>Actor: co_await logic_func(actor, ProtoMsg)
```

**流程 B：高并发出站与零拷贝批处理 (The Outbound Workflow)**

展示多线程竞争发送时，如何通过 CAS 和 Batcher 实现无锁化的高效合并发送。

代码段

```
sequenceDiagram
    participant Actor1 as Worker Thread 1
    participant Actor2 as Worker Thread 2
    participant Conn as Connection
    participant Batcher as OutboxBatcher
    participant OS as io_uring (Kernel)

    Actor1->>Conn: send(packet_A) (Push to Outbox + SpinLock)
    Actor1->>Conn: CAS(in_pending) -> True (Register to Env)
    Actor2->>Conn: send(packet_B) (Push to Outbox + SpinLock)
    Actor2->>Conn: CAS(in_pending) -> False (Skip register)
    
    Note over Conn: Env Event Loop 触发 flush()
    Conn->>Conn: send_batch_coro() starts
    Conn->>Conn: Swap Outbox to local batch
    Conn->>Batcher: prepare_batch(batch)
    Note over Batcher: 构建 iovec[A_hdr, A_body, B_hdr, B_body]
    
    Conn->>OS: co_await socket_.send(iovecs)
    OS-->>Conn: Resume (Bytes Written)
    Conn->>Batcher: advance(bytes) (处理 Partial Write)
    Note over Conn: Batch 清空，Packet 自动归还 Pool
```

## 4. 关键技术决策 (Key Design Decisions)

|   |   |   |   |
|---|---|---|---|
|**技术点**|**决策理由 (Why)**|**收益 (Benefit)**|**代价/取舍**|
|**Proactor (io_uring)**|取代传统的 Reactor (epoll)。|**纯异步 IO**，系统调用开销降至最低；协程栈天然保活，代码逻辑从“回调地狱”变为线性。|放弃 Linux < 5.1 内核兼容性。|
|**SBO + TCMalloc Pool**|Packet 内置 1KB 栈内存并由 Common 层对象池管理。|**零堆内存分配 (Zero-Allocation)**。99% 的包在连续内存读写，**L1 Cache Friendly**。|内存占用略增（以空间换时间）。|
|**Send/Flush Isolation**|业务线程只入队，不写 Socket。|避免多线程并发写 Socket 的锁竞争与乱序；实现 **Gather I/O (writev)** 批量发送。|增加了发送路径的延迟（微秒级），换取极致吞吐。|
|**类型擦除路由**|`Concepts` + Lambda 闭包。|运行时无反射、**无读写锁 (RWLock)**，实现 O(1) 静态路由分发。|路由表必须在启动阶段注册完毕，不支持动态热加载。|

## 5. 接口示例 (API Usage)

```
// 1. 初始化：注册强类型业务 Handler
// 亮点：利用 C++20 模板自动推导，直接使用 Proto 类型
net::Dispatcher::instance().register_handler<CSLoginReq>(
    1001, // MsgID
    [](core::Actor* actor, const CSLoginReq& req) -> core::Task<void> {
        Log::info("Login: {}", req.username());
        co_return;
    }
);

// 2. 启动服务 (Proactor 模式)
net::Acceptor acceptor(8080);
while (true) {
    // 异步等待，无阻塞
    net::Socket client = co_await acceptor.accept();
    auto conn = std::make_shared<net::Connection>(std::move(client));
    Env::spawn(conn->start_loop());
}

// 3. 业务发送 (零拷贝)
void on_login_success(shared_ptr<net::Connection> conn) {
    SCLoginResp resp;
    resp.set_code(0);
    
    // Acquire from Pool (Zero-Alloc)
    auto packet = net::PacketPool::instance().acquire();
    packet->pack_into(2001, resp); 
    
    // Enqueue only (Zero-Copy)
    conn->send(std::move(packet)); 
}
```