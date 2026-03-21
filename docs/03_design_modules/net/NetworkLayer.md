

### Aegis 引擎网络层：全异步零拷贝通信架构 (Network Layer)

#### 1. 模块全景图 (The Big Picture)

- **一句话定位：** Aegis 网络层是一个基于 `io_uring` 和 C++20 协程构建的**全异步、零分配、零拷贝**的高并发传输底座。
    
- **定位：** Engine Core (引擎核心层)。它是承接 OS 内核与上层 Actor 逻辑系统的关键枢纽。
    
- **职责边界：**
    
    - **负责：** TCP 连接生命周期管理、粘包/拆包处理（Framing）、无锁化批量收发、基于 `iovec` 的内存视图构建、基于协议号的强类型路由分发。
        
    - **不负责：** 业务逻辑状态维护、Actor 调度执行、协议结构体本身的定义（由 Protobuf 负责）。
        
- **依赖关系：**
    
    - **向下依赖：** Linux Kernel (`io_uring`, `Socket API`), C++20 协程标准库, 引擎基础库 (`aegis::common` 的锁与内存池)。
        
    - **向上支撑：** 为 `Actor System` 提供透明的、强类型的消息投递与触发能力。
        

#### 2. 架构与类图 (Architecture & Topology)

网络层在物理结构上被划分为三大核心组块：**连接接入组**、**数据载体组**、**路由分发组**。

代码段

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

**数据流向 (Data Flow)：**

- **Inbound (入站)：** 网卡 -> OS 缓冲区 -> `io_uring` CQE -> `Socket::recv` (协程恢复) -> `Connection::read_packet` (拆包) -> `Dispatcher::dispatch` -> 业务 `Actor`。
    
- **Outbound (出站)：** 业务 `Actor` -> `Connection::send` (入队 Outbox) -> Env 唤醒触发 `flush` -> `OutboxBatcher` (构建散列视图) -> `Socket::send` -> `io_uring` SQE -> 网卡。
    

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

#### 4. 关键技术决策与取舍 (Key Design Decisions)

- **模型选择：Proactor (`io_uring`) vs Reactor (`epoll`)**
    
    - **决策：** 全面拥抱 `io_uring` 与 C++20 协程的深度绑定。放弃常规的 `epoll` + `O_NONBLOCK` + 状态机回调模型。
        
    - **取舍：** 放弃了对旧版 Linux 内核（< 5.1）的兼容性，换取了彻底免除应用层非阻塞 `read/write` 系统调用的开销，并通过协程栈天然保活局部变量，将复杂的 TCP 粘包逻辑化简为线性的 `while` 循环。
        
- **内存策略：SBO (小包优化) + 多级缓存池**
    
    - **决策：** `Packet` 内置 1024 字节连续栈/成员内存，并由 `TCMalloc` 风格的 L1/L2 对象池全局托管。
        
    - **收益：** 实现了核心热路径上的 **零堆内存分配 (Zero-Allocation)**。99% 的网络包直接在连续内存中完成读写，极其契合 CPU 的 L1 Cache 预取机制（Cache Friendly），彻底消除了指针追逐（Pointer Chasing）和 `malloc` 全局锁竞争。
        
- **并发策略：发送排空分离 (Send/Flush Isolation)**
    
    - **决策：** 上层多线程 Actor 调用 `Connection::send` 时，绝不直接触发 `socket_.send`，而是仅做极轻量的无锁队列入队（自旋锁+CAS状态机）。
        
    - **收益：** 通过单个后台协程独占执行 `flush`，避免了多线程并发写 Socket 的乱序灾难。 配合 `OutboxBatcher`，将多个离散的 Packet 聚合成一个连续的 `iovec` 数组提交给内核，实现了极致的 **零拷贝 (Zero-Copy)** 批量发送。
        
- **路由策略：生命周期隔离与类型擦除**
    
    - **决策：** `Dispatcher` 采用 Concepts 编译期校验和 Lambda 闭包进行类型擦除。规定路由表仅在服务器初始化阶段可写，运行时绝对只读。
        
    - **收益：** 彻底去除了热路径上的读写锁（RWLock），实现了多线程 O(1) 零争用的极速路由分发。
        

#### 5. 对外接口与用法 (API & Usage)

网络层对业务逻辑层屏蔽了所有底层字节操作，提供了强类型、极其简洁的协程接口：

C++

```
// 1. 初始化阶段：注册强类型业务 Handler (自动完成 Protobuf 反序列化)
net::Dispatcher::instance().register_handler<CSLoginReq>(
    1001, // 协议 MsgID
    [](core::Actor* actor, const CSLoginReq& req) -> core::Task<void> {
        Log::info("User {} logging in...", req.username());
        // 处理业务逻辑...
        co_return;
    }
);

// 2. 启动服务：绑定端口并开始接收连接
net::Acceptor acceptor(8080, "0.0.0.0");
while (true) {
    // 异步等待新连接 (协程挂起)
    net::Socket client_socket = co_await acceptor.accept();
    
    // 创建 Connection 容器并挂载到 Actor System
    auto conn = std::make_shared<net::Connection>(std::move(client_socket));
    Env::instance().spawn_connection_loop(conn); 
}

// 3. 业务下发：在任意 Actor 中向客户端发送数据
void on_login_success(std::shared_ptr<net::Connection> conn) {
    SCLoginResp resp;
    resp.set_code(0);
    
    // 从池中获取可用封包，零分配
    auto packet = net::PacketPool::instance().acquire();
    packet->pack_into(2001, resp); 
    
    // 线程安全投递，后台自动批量聚合、零拷贝发送
    conn->send(std::move(packet)); 
}
```