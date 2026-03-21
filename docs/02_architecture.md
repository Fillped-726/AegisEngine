## 1. 设计理念 (Design Philosophy)

Aegis 是一个基于 **C++20 Coroutines** 和 **Linux io_uring** 构建的高性能、全异步游戏服务器引擎。它并非通过简单的堆砌开源库来实现，而是自底向上重构了从内存分配到业务调度的完整技术栈。

本架构的设计遵循以下核心原则：

- **Everything is Asynchronous (全异步):** 利用 C++20 协程彻底消除“回调地狱”，将异步逻辑写成同步代码，同时保持非阻塞的高并发能力。
    
- **Zero-Overhead Abstraction (零开销抽象):** 核心热路径（Hot Path）上拒绝虚函数、拒绝锁竞争、拒绝不必要的内存拷贝。
    
- **Share Nothing (无共享):** 业务层采用 Actor 模型，通过消息驱动完全隔离状态，从根本上规避多线程死锁与竞态条件。
    
- **Cache Friendly (缓存友好):** 极致优化数据布局（Data Layout），利用连续内存（Intrusive List, Flat Grid）最大化 CPU L1/L2 缓存命中率。
    

---

## 2. 架构分层全景图 (Layered Architecture)

Aegis 采用严格的分层架构，层级之间遵循**单向依赖**原则（上层依赖下层，下层对上层一无所知）。

代码段

```
graph TD
    %% 定义样式
    classDef service fill:#e1f5fe,stroke:#01579b,stroke-width:2px;
    classDef actor fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px;
    classDef runtime fill:#fff3e0,stroke:#ef6c00,stroke-width:2px;
    classDef net fill:#f3e5f5,stroke:#7b1fa2,stroke-width:2px;
    classDef common fill:#eceff1,stroke:#455a64,stroke-width:2px;
    classDef kernel fill:#fafafa,stroke:#9e9e9e,stroke-width:1px,stroke-dasharray: 5 5;

    subgraph UserSpace [Aegis Engine User Space]
        direction TB
        
        %% L5 Service
        subgraph L5 [Phase 5: Service Layer (应用服务层)]
            Gate[Gate Server]
            Proto[Protobuf Protocols]
            Bind[Lambda Binding]
        end
        
        %% L4 Actor
        subgraph L4 [Phase 4: Actor Framework (业务逻辑层)]
            Reg[Actor Registry]
            Mail[Mailbox & Messaging]
            AOI[AOI & Supervision]
        end
        
        %% L3 Runtime
        subgraph L3 [Phase 3: Core Runtime (核心运行时)]
            Sched[M:N Scheduler]
            Env[IO Environment]
            Timer[Hierarchical Timer]
        end
        
        %% L2 Net
        subgraph L2 [Phase 2: Network Layer (网络传输层)]
            Conn[Connection & Batching]
            Pkt[Packet Pool]
            Disp[Dispatcher]
        end
        
        %% L1 Common
        subgraph L1 [Phase 1: Common Infrastructure (基础组件层)]
            Mem[ObjectPool & TLS]
            Queue[WorkStealingQueue]
            List[Intrusive List]
        end
    end

    subgraph KernelSpace [Linux Kernel Space]
        IO[io_uring]
        Net[TCP Stack]
        Time[Hardware Clock]
    end

    %% 依赖关系
    L5 --> L4
    L4 --> L3
    L3 --> L2
    L2 --> L1
    L2 -.-> IO
    L3 -.-> IO
    
    %% 样式应用
    class Gate,Proto,Bind service;
    class Reg,Mail,AOI actor;
    class Sched,Env,Timer runtime;
    class Conn,Pkt,Disp net;
    class Mem,Queue,List common;
    class IO,Net,Time kernel;
```

---

## 3. 模块详解 (Modules Breakdown)

为了保证系统的稳定性与极致性能，Aegis 的构建采用了**自底向上 (Bottom-Up)** 的工程方法：

### **Phase 1: Common Infrastructure (地基)**

- **定位:** 提供无需系统调用（Syscall-free）的底层原语。
    
- **核心组件:**
    
    - **ObjectPool:** 配合 Thread-Local Cache (TLS) 实现无锁内存分配，解决 `malloc/free` 的全局锁竞争。
        
    - **WorkStealingQueue:** 基于 Chase-Lev 算法的无锁双端队列，支撑高效的任务调度。
        
    - **IntrusiveList:** 侵入式链表，提供零内存分配的对象管理能力。
        

### **Phase 2: Network Layer (动脉)**

- **定位:** 处理所有 TCP 连接与字节流，向上屏蔽 IO 细节。
    
- **核心技术:**
    
    - **io_uring Proactor:** 利用 Linux 5.10+ 的异步 IO 接口，通过 SQPOLL 实现零系统调用提交。
        
    - **Zero-Copy Send:** 利用 `Outbox` 机制聚合发送包，通过 `iovec` 向量化写入，避免内核态拷贝。
        
    - **Packet Management:** 结合 Phase 1 的内存池，实现网络包的“借用-填充-发送-归还”全生命周期零分配。
        

### **Phase 3: Core Runtime (心脏)**

- **定位:** 系统的动力源，负责 CPU 算力分配与 IO 事件驱动。
    
- **核心机制:**
    
    - **M:N Scheduling:** 将成千上万个协程（Tasks）动态映射到少量的物理线程（Workers）上执行。
        
    - **Work Stealing:** 自动平衡多核负载，解决“一核有难，八核围观”的问题。
        
    - **Symmetric Transfer:** 利用 C++20 协程的对称转移特性，消除递归调用带来的栈溢出风险。
        

### **Phase 4: Actor Framework (大脑)**

- **定位:** 业务逻辑的容器，提供高并发下的编程范式。
    
- **核心模型:**
    
    - **Share Nothing:** Actor 之间状态隔离，通过消息通信，彻底移除业务层互斥锁 (`std::mutex`)。
        
    - **Generational Index:** 使用 `Index + Version` 的强句柄机制，解决 Actor 销毁后的悬垂指针与 ABA 问题。
        
    - **Lock-Free Mailbox:** 基于 MPSC 队列的消息投递，保证高并发下的写入性能。
        

### **Phase 5: Service Layer (面子)**

- **定位:** 最终的应用组装与协议契约。
    
- **核心功能:**
    
    - **Protobuf Binding:** 利用模板元编程，将网络消息 ID 静态绑定到具体的业务 Lambda。
        
    - **Stateful Gateway:** 网关进程直接持有状态，实现零 RPC 调用的极低延迟响应。
        

---

## 4. 线程模型与数据流 (Threading & Data Flow)

Aegis 采用了 **IO 线程与 Worker 线程分离** 的设计，最大化利用多核优势。

### 线程角色

1. **Main Thread (1个):** 负责启动、配置加载、信号处理、主循环监控。
    
2. **IO Thread (1个):** 独占 `io_uring` 实例，负责所有网络 I/O 的提交与收割（Reap）。它不处理任何业务逻辑，只负责搬运数据。
    
3. **Worker Threads (N个):** 数量通常等于 CPU 物理核心数。它们负责运行 `Scheduler`，执行 Actor 逻辑、定时器回调、协议解析。
    

### 核心数据流 (The Pipeline)

一个典型的网络请求处理流程如下：

Plaintext

```
[NIC 网卡] 
    ↓ (DMA)
[Kernel RingBuffer]
    ↓ (io_uring CQE)
[IO Thread] 
    → 收割完成事件，唤醒 Connection 协程
    ↓ (Dispatch)
[Worker Thread A] 
    → 解析 Packet Header，路由 MsgID
    → 查找目标 Actor
    → 将消息 Push 到 Actor Mailbox (若 Actor 处于 Idle，则将其放入调度队列)
    ↓ (Schedule)
[Worker Thread B] (可能发生窃取)
    → 取出 Actor，批量处理 Mailbox 消息
    → 执行 Protobuf 反序列化 & 业务 Lambda
    → 修改 Actor 内存状态 (无锁)
    → 发送回包 (Push 到 Connection Outbox)
    ↓ (Flush)
[IO Thread]
    → 收集所有 Connection 的 Outbox
    → 聚合成 iovec 数组
    → 写入 io_uring SQE
```

---

## 5. 技术栈清单 (Tech Stack)

- **Language Standard:** C++20 (Concepts, Coroutines, JThread)
    
- **OS Kernel:** Linux 5.10+ (io_uring support required)
    
- **Build System:** CMake 3.15+
    
- **Compiler:** GCC 11+ / Clang 14+
    
- **3rd Party Libraries:**
    
    - `protobuf` (v3.0+): 序列化协议
        
    - `spdlog`: 高性能异步日志
        
    - `liburing`: Linux 内核接口封装
        
    - `googletest`: 单元测试框架