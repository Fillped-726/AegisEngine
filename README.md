# Aegis Engine 🛡️

**Aegis** 是一个基于 **C++20 Coroutines** 和 **Linux io_uring** 自主研发的高性能、全异步游戏服务器框架。

该项目旨在解决 I/O 密集型 MMO 游戏场景下的 **C10K/C100K 并发难题**。它摒弃了传统的 Epoll/Reactor 模型，采用内核级异步 **Proactor (io_uring)** 结合用户态 **M:N 协程调度**，并实现了基于 **无锁消息队列** 的 **Actor 模型**，为复杂的业务逻辑提供了一个零锁竞争、高吞吐的执行环境。

---

## 🚀 核心技术亮点 (Core Tech Stack)

### 1. 网络传输层 (Network Layer - The Arteries)

- **极致异步 IO:** 基于 **Linux 5.10+ io_uring** 构建 Proactor 模型，开启 `IORING_SETUP_SQPOLL` 特性，在热路径（Hot Path）上实现了 **零系统调用 (Zero-Syscall)**。
    
- **零拷贝发送:** 实现了 `OutboxBatcher` 机制，利用 `iovec` 将多个逻辑包在内核层聚合发送，彻底消除了用户态到内核态的冗余内存拷贝。
    
- **全生命周期无锁:** 自研线程本地 **PacketPool**，实现了网络包从“分配-填充-发送-归还”全流程的 **Zero-Allocation**。
    

### 2. 核心运行时 (Core Runtime - The Heart)

- **M:N 协程调度:** 手写调度器，将海量用户态协程（Tasks）动态映射到固定数量的物理线程（Workers）上执行。
    
- **工作窃取算法:** 实现了 **Chase-Lev Deque** 无锁双端队列，支持多核间的任务负载均衡（Work Stealing），有效解决“一核有难，多核围观”的 CPU 饥饿问题。
    
- **对称转移优化:** 利用 C++20 `std::coroutine_handle` 实现协程间的 **Symmetric Transfer**，利用尾调用优化消除递归栈溢出风险。
    

### 3. Actor 业务框架 (Actor Framework - The Brain)

- **Share-Nothing 架构:** 严格遵循 Actor 模型，实体间仅通过消息通信，业务逻辑层彻底移除了互斥锁 (`std::mutex`)。
    
- **无锁邮箱:** 基于 **Intrusive MPSC Queue** 实现的高性能消息邮箱，大幅提升高并发下的消息投递吞吐。
    
- **代际索引句柄:** 引入 `Generational Index` (ID + Version) 机制，完美解决了 Actor 动态生命周期中的 **ABA 问题** 与悬垂指针风险。
    

### 4. 应用服务层 (Gameplay Services - The Face)

- **静态反射绑定:** 利用模板元编程技术，实现了 **Protobuf** 消息 ID 到 C++ Lambda 的编译期强类型绑定。
    
- **高效 AOI 算法:** 实现了基于 **扁平网格 (Flat Grid)** 的关注点管理算法，相比四叉树方案具有极佳的 CPU 缓存局部性 (Cache Locality)。
    

---

## 📊 Performance Benchmark

### Environment & Configuration
* **Hardware:** WSL2 (4 Cores, 8GB RAM).
* **Core Affinity Strategy:**
    * **Core 0:** Userspace IO Thread (Event Loop).
    * **Core 1:** Kernel `io_uring` SQPOLL Thread.
    * **Core 2-3:** Logic Worker Threads (Actor Runtime).
* **Scenario:** Broadcast Storm (AOI Stress Test).

### Benchmark Results (Scaling)

| Metric | 400 Bots (Light) | 600 Bots (Optimal) | 800 Bots (Peak) |
| :--- | :--- | :--- | :--- |
| **Throughput (PPS)** | ~300,000 | ~650,000 | **1,158,914** 🚀 |
| **Bandwidth** | ~5.5 MB/s | ~12 MB/s | **21.6 MB/s** |
| **Latency Avg** | **10.2 ms** | 19.5 ms | 31.2 ms |
| **Latency P99** | **18.7 ms** | **35.6 ms** | 58.3 ms |
| **Logic Ops/s** | 38k | 80k | **138k** |

> **Analysis:**
> 1.  **E-Sports Grade Latency:** At 400 bots, the P99 latency stays under **20ms**, ideal for fast-paced competitive games.
> 2.  **1 Million PPS Barrier:** At peak load (800 bots), the engine successfully processes over **1.15 Million packets per second** on just 4 cores, fully utilizing the `io_uring` Zero-Copy pipeline.
> 3.  **Graceful Degradation:** Even under saturation, the Work-Stealing scheduler prevents latency spikes, keeping Max Latency controlled (~100ms).

```
graph TD
    subgraph CPU_Affinity [WSL CPU Core Binding Strategy]
        direction TB
        C0[Core 0: IO Thread] -->|Submit/Reap| URING[io_uring CQ/SQ]
        C1[Core 1: Kernel SQPOLL] -.->|Polls| URING
        URING -->|Dispatch Packets| C2
        URING -->|Dispatch Packets| C3
        
        subgraph Workers [Logic Workers]
            direction LR
            C2[Core 2: Worker 1] 
            C3[Core 3: Worker 2]
        end
        
        C2 <-->|Work Stealing| C3
    end
    
    style C0 fill:#ffccbc,stroke:#d84315
    style C1 fill:#ffe0b2,stroke:#ef6c00
    style C2 fill:#c8e6c9,stroke:#2e7d32
    style C3 fill:#c8e6c9,stroke:#2e7d32
```
---

## 🏗️ 架构全景图

Aegis 采用严格的分层架构设计，确保模块解耦与性能隔离。

代码段

```
graph TD
    User[Game Clients] -->|TCP/Protobuf| L5
    subgraph Engine [Aegis Engine]
        L5[Phase 5: Service Layer 应用服务层] -->|Dispatch| L4
        L4[Phase 4: Actor Framework 业务逻辑层] -->|Schedule| L3
        L3[Phase 3: Core Runtime 核心运行时] -->|Drive| L2
        L2[Phase 2: Network Layer 网络传输层] -->|Alloc| L1
        L1[Phase 1: Common Infra 基础组件层]
    end
    L2 -.->|io_uring| Kernel[Linux Kernel]
```

---

## 🛠️ 快速开始 (Quick Start)

### 依赖环境

- **编译器:** GCC 11+ 或 Clang 14+ (必须支持 C++20)
    
- **操作系统:** Linux Kernel 5.10+ (必须支持 `io_uring`)
    
- **构建工具:** CMake 3.15+
    

### 编译构建

Bash

```
# 1. 克隆仓库
git clone https://github.com/your-username/AegisEngine.git
cd AegisEngine

# 2. 编译 Release 版本 (开启 -O3)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 运行压测

本项目提供了一键压测工具，用于复现上述性能数据。

**1. 启动网关服务 (绑定前4核)**

Bash

```
# Terminal 1
sudo taskset -c 0-3 ./services/gate/gate_server
```

**2. 启动压测机器人 (绑定后4核)**

Bash

```
# Terminal 2
sudo taskset -c 4-7 ./tests/bench_battle
```

---

## 📂 项目结构

Plaintext

```
.
├── include/aegis
│   ├── common/      # 底层原语 (ObjectPool, WSQueue, SpinLock)
│   ├── net/         # 网络 IO (io_uring, Connection, Batcher)
│   └── core/        # 运行时 (Scheduler, Actor, Timer)
├── services/
│   └── gate/        # 示例网关服务器实现
├── shared/proto/    # Protobuf 协议定义
├── src/             # 核心库实现
├── tests/           # 单元测试与压测工具
└── CMakeLists.txt
```

---

## 📄 开源协议

本项目采用 MIT 协议开源 - 详见 [LICENSE](https://www.google.com/search?q=LICENSE) 文件。

---

**Author:** [郝光磊]

**Email:** [360649459@qq.com]

_(2026 届校招求职中)_