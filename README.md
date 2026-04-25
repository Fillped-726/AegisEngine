# Aegis Engine 🛡️

> Linux 内核级异步游戏服务器框架 · C++20 Coroutines + io_uring Proactor · M:N 协程 Actor 模型

Aegis Engine 是一个从零自研的高性能全异步 MMO 游戏服务器框架。它没有依赖任何第三方网络库（如 Boost.Asio、libuv），直接在 `io_uring` sqpoll 模式上封装了 Proactor 模型，结合 C++20 协程和 Actor 架构，实现了**热路径零系统调用、零锁业务逻辑、百微秒级调度**。

| 指标 | 400 Bot (轻载) | 600 Bot (最佳) | 800 Bot (峰值) |
|:---|:---|:---|:---|
| **吞吐量 (PPS)** | ~300,000 | ~650,000 | **1,158,914** 🚀 |
| **带宽 (MB/s)** | ~5.5 | ~12 | ~21.6 |
| **Avg 延迟 (ms)** | 10.2 | 19.5 | 31.2 |
| **P99 延迟 (ms)** | 18.7 | 35.6 | 58.3 |
| **逻辑运算 (ops/s)** | 38k | 80k | 138k |

> 测试环境: WSL2 (4C/8GB) · Core 0 IO 线程 + Core 1 SQPOLL 内核线程 + Core 2~3 业务 Worker

---

## 目录

- [技术栈](#技术栈)
- [架构总览](#架构总览)
- [核心设计要点](#核心设计要点)
- [快速开始](#快速开始)
- [项目结构](#项目结构)
- [协议规范](#协议规范)
- [测试与基准](#测试与基准)
- [产品化路线图](#产品化路线图-godot-客户端)
- [开发者](#开发者)

---

## 技术栈

| 层 | 技术 | 说明 |
|:---|:---|:---|
| 语言 | C++20 | Coroutines (TS), Concepts, if constexpr, std::endian |
| 异步 IO | io_uring + SQPOLL | 热路径零系统调用，内核态轮询完成队列 |
| 协程 | `Task<T>`, `DetachedTask` | 手写 Promise Type，对称转移 (Symmetric Transfer) |
| 调度 | Thread-per-Core + Work Stealing | Chase-Lev 无锁双端队列，任务窃取负载均衡 |
| Actor | 无锁邮箱 + 代际句柄 | Intrusive MPSC Queue, Generational Index (ABA safe) |
| 序列化 | Protobuf | 编译期消息 ID 到 Lambda 的静态反射绑定 |
| 日志 | spdlog | 异步日志，日志级别运行时可调 |
| 构建 | CMake + vcpkg | 自动管理依赖 (liburing, protobuf, spdlog, fmt, jemalloc...) |
| 内存 | jemalloc + ObjectPool | 全局 jemalloc 减少碎片；消息/网络包走私有对象池实现零分配 |
| 认证 | OpenSSL | gRPC 安全信道 |

---

## 架构总览

```
┌───────────────────────────────────────────────────────────────────┐
│                   Phase 5: 服务层 (Services)                      │
│                                                                   │
│   GateServer (端口 8888)                                         │
│   ├── accept_loop (Worker 0) —— Round-Robin 分发连接             │
│   ├── handle_session (Worker 1~3)                                │
│   │   ├── PlayerActor 创建                                       │
│   │   ├── read_packet → dispatch_to_actor 循环                   │
│   │   └── 断开 → SessionClosedMsg → ActorDiedMsg                 │
│   ├── RoomManager 管理房间                                       │
│   └── SceneActor 管理场景 (500×500, 网格 10×10)                 │
├───────────────────────────────────────────────────────────────────┤
│                   Phase 4: Actor 框架 (Framework)                 │
│                                                                   │
│   Actor ←─────────────────────────────────────────── 消息邮箱     │
│   ├── PlayerActor (网络感知 Actor)                               │
│   ├── SceneActor   (场景 + NPC 管理 + AOI)                      │
│   ├── NpcActor      (AI 实体)                                   │
│   ├── RoomManager   (房间生命周期)                              │
│   └── ActorRegistry (O(1) 路由 · 65K 容量 · 无锁读)             │
│                                                                   │
│   消息流:                                                        │
│   协程 Msg ──→ 网络 Msg ──→ 场景 Msg ──→ RPC Msg ──→ 销毁 Msg  │
│   BasicMessage<Derived, TypeId> (CRTP 自动注册 finalizer)        │
│   NetworkMessage (Pooled)                                        │
│   RpcMessage<ReqT, ResT> (带 std::promise 同步等待)             │
├───────────────────────────────────────────────────────────────────┤
│                   Phase 3: 核心运行时 (Runtime)                   │
│                                                                   │
│   Scheduler (单例)                                               │
│   ├── start(num_workers)                                         │
│   ├── Round-Robin 初始分发                                       │
│   └── ┌──────────────────────────────────────┐                   │
│       │        Worker 0 (IO 线程)            │                   │
│       │  ┌────────────────────────────────┐   │                   │
│       │  │ io_uring SQ/CQ                  │   │                   │
│       │  │ Local Run Queue (Chase-Lev)   │   │                   │
│       │  │ Timer Wheel  (50ms 精度)      │   │                   │
│       │  └────────────────────────────────┘   │                   │
│       └──────────────────────────────────────┘                   │
│       ┌──────────────────────────────────────┐                   │
│       │   Worker 1~3 (业务线程)            │                   │
│       │  ┌────────────────────────────────┐   │                   │
│       │  │ Local Run Queue (Chase-Lev)   │◄──│── Work Steal      │
│       │  │ Actor 协程执行                  │   │                   │
│       │  └────────────────────────────────┘   │                   │
│       └──────────────────────────────────────┘                   │
├───────────────────────────────────────────────────────────────────┤
│                   Phase 2: 网络传输层 (Network)                    │
│                                                                   │
│   Acceptor  ←── listen → accept() → Socket                       │
│   Connection ←── read_packet() → PooledPacket                    │
│   ├── K_HEADER_SIZE = 4Bytes (Length + MsgID 大端序)            │
│   ├── rx_buffer_ + rx_len_ 协程零拷贝拆包                        │
│   ├── 100% 零拷贝发送: OutboxBatcher (iovec 聚合)               │
│   └── Packet SBO: 1KB stack_buf_ / heap fallback                 │
├───────────────────────────────────────────────────────────────────┤
│                   Phase 1: 基础设施 (Common)                      │
│                                                                   │
│   ObjectPool<NetworkMessage, 100K> · PacketPool<PooledPacket>    │
│   IntrusiveList<TimerNode> · SpinLock (TAS + PAUSE backoff)      │
│   HierarchicalTimeWheel (5 级 256+64*4 槽, O(1))                │
│   WorkStealingQueue (Chase-Lev Deque)                            │
│   ConcurrentQueue (MoodyCamel)                                   │
└───────────────────────────────────────────────────────────────────┘
```

### CPU 绑定策略

```
┌──────────────────────────────────────────────────────────┐
│                  WSL 4-Core 拓扑                          │
│                                                          │
│  Core 0    Core 1         Core 2        Core 3           │
│  ┌──────┐  ┌──────┐      ┌──────┐      ┌──────┐        │
│  │ IO   │  │ SQ   │      │ Wkr1 │◄────►│ Wkr2 │        │
│  │Thread│  │POLL  │      │Actor │ Work │Actor │        │
│  │uring │  │Kernel│      │Logic │ Steal│Logic │        │
│  └──────┘  └──────┘      └──────┘      └──────┘        │
│       │         │             │            │            │
│       └────┬────┘             └─────┬──────┘            │
│            │                       │                    │
│      io_uring CQ/SQ          Actor Messages             │
│      (Submit+Reap)          (无锁邮箱/跨核)             │
└──────────────────────────────────────────────────────────┘
```

---

## 核心设计要点

### 1. 零系统调用网络 IO

使用 `io_uring` 的 `IORING_SETUP_SQPOLL` 特性：内核线程在固定 CPU 上轮询提交队列 (SQ)，用户态 IO 线程仅需读完成队列 (CQ) 即可获取已完成的异步操作结果。在 800 Bot 的广播风暴测试中，每秒处理 115 万个数据包，IO 线程的 CPU 占用率不到 5%。

### 2. 协程友好的全异步编程

无需回调，不依赖 Boost.Asio。手写 `Task<T>` Promise Type 支持：

```cpp
// 读取一个完整的网络包 —— 协程在数据到达前自动挂起
Task<PooledPacket> Connection::read_packet() { ... }

// 在 Worker 上驱动 Acceptor
DetachedTask GateServer::accept_loop(int port) {
    Acceptor acceptor(port);
    while (true) {
        Socket client = co_await acceptor.accept();
        // Round-Robin 分发到业务 Worker
    }
}
```

### 3. 无锁 Actor 模型

- **ActorID** = 64-bit (32-bit index + 32-bit version)，使用 `std::memory_order_release/acquire` 保证可见性，彻底解决 ABA 问题
- **消息邮箱** = 侵入式 MPSC (Multiple Producer, Single Consumer) 队列，Actor 在同一 Worker 上时 `push()` 直接入队，跨 Worker 时通过 `post_cross_core_task()` 原子入队
- **业务逻辑零互斥锁** — 一个 Actor 永远只被一个 Worker 线程处理

### 4. 时间轮定时器

分层时间轮 (Hierarchical TimeWheel) 替代传统 `std::priority_queue`：
- 5 级轮 (256 + 4×64 槽)，`O(1)` 添加/取消/滴答
- 基于侵入式链表，定时器节点从 `TimerNodePool` 分配
- 精度 50ms，覆盖范围 ~2^29 ticks (≈ 2.7 年)

### 5. 零拷贝发送管道

```
业务层发送 → OutboxBatcher 收集 → io_uring WRITEV (iovec) → 内核 TCP 层
```

多个逻辑包在 Worker 线程本地聚合后通过一次 `WRITEV` 提交，减少系统调用和内存拷贝。空闲时自动 Shrink rx_buffer_ 到 4KB。

### 6. 静态反射 Handler 绑定

```protobuf
enum MsgID {
    CS_LOGIN_REQ = 1001;
    CS_MOVE_REQ  = 2003;
    CS_SKILL_CAST_REQ = 2010;
}
```

`handler_loader.h` 在编译期将 `MsgID` 映射到 `Handler` 函数指针，无需运行期 `switch-case` 或 `std::map` 查找。

---

## 快速开始

### 前置要求

- **OS**: Linux 5.10+ (io_uring 支持)
- **Compiler**: GCC 11+ 或 Clang 14+ (C++20 完整支持)
- **CMake**: 3.15+
- **vcpkg**: 见下方 `vcpkg bootstrap`

### 编译

```bash
# 1. 安装依赖 (如尚未配置 vcpkg)
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg

# 2. 克隆并构建 AegisEngine
git clone https://github.com/your-username/AegisEngine.git
cd AegisEngine
mkdir build && cd build

cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
make -j$(nproc)
```

### 运行

```bash
# Terminal 1: 启动网关服务 (绑定 Core 0~3)
sudo taskset -c 0-3 ./services/gate/gate_server

# Terminal 2: 启动压测机器人 (绑定 Core 4~7)
sudo taskset -c 4-7 ./tests/bench_battle
```

---

## 项目结构

```
AegisEngine/
├── CMakeLists.txt                  # 顶级构建定义
├── README.md                       # 本文件
├── CHANGELOG.md                    # 版本变更日志
│
├── include/aegis/                  # 公开头文件 (Public API)
│   ├── common/                     # 基础设施
│   │   ├── objectPool.h            # 泛型对象池 (固定/增长策略)
│   │   ├── spinLock.h              # 自旋锁 (TAS + PAUSE)
│   │   ├── task.h                  # Task<T> / DetachedTask 协程原语
│   │   ├── intrusive_list.h        # 侵入式双向链表
│   │   ├── ws_queue.h              # Chase-Lev Work-Stealing Deque
│   │   ├── aegisLog.h              # spdlog 日志封装
│   │   └── tools.h                 # 工具函数 (时间戳, 随机数)
│   │
│   ├── net/                        # 网络传输层
│   │   ├── socket.h                # RAII Socket 封装
│   │   ├── acceptor.h              # 异步 TCP Acceptor (co_await 支持)
│   │   ├── connection.h            # Connection 会话 (read_packet/send/flush)
│   │   ├── ring.h                  # io_uring 封装 (SQ/CQ 管理)
│   │   ├── packet.h                # Packet (SBO 1KB + 大端序序列化)
│   │   ├── packetPool.h            # PooledPacket RAII 包装
│   │   └── outbox_batcher.h        # 批量发送 (iovec 聚合)
│   │
│   └── core/                       # 核心运行时
│       ├── actor.h                 # Actor 基类 + 无锁邮箱
│       ├── actor_registry.h        # 全局 Actor 注册表 (65K 容量)
│       ├── playerActor.h           # 玩家 Actor (网络绑定)
│       ├── scene_actor.h           # 场景 Actor (NPC + AOI)
│       ├── npc_actor.h             # NPC Actor
│       ├── room_manager.h          # 房间管理器
│       ├── message.h               # ActorMessage 体系 (CRTP + 自动注册)
│       ├── scheduler.h             # Thread-per-Core 调度器
│       ├── worker.h                # Worker 线程 (本地队列 + io_uring)
│       ├── hierarchy_timer.h       # 分层时间轮 (5 级, O(1))
│       └── aoi_grid.h              # 扁平网格 AOI (Catch-Locality 友好)
│
├── src/                            # 核心库实现
│   └── aegis/
│       ├── common/                 # 基础设施实现
│       ├── net/                    # 网络层实现
│       └── core/                   # Actor 运行时实现
│
├── services/                       # 可部署的服务
│   └── gate/                       # 网关服务 (GateServer)
│       ├── CMakeLists.txt
│       ├── include/gate_server.h   # GateServer 类定义
│       └── src/
│           ├── main.cpp            # 入口: init → run(8888)
│           ├── gate_server.cpp     # accept_loop → handle_session
│           └── handler_loader.h    # 静态反射 Handler 绑定
│
├── shared/                         # 跨服务共享代码
│   ├── proto/                      # Protobuf 协议定义
│   │   ├── ids.proto              # MsgID 枚举 (1001~3002)
│   │   ├── cs_lobby.proto         # 大厅协议 (LoginReq/Res)
│   │   ├── cs_battle.proto         # 战斗协议 (Move/Skill/Damage)
│   │   ├── ss_bridge.proto         # 服务间通信 (Room 管理 RPC)
│   │   └── common.proto           # 通用类型 (PBVector3)
│   └── pb/                        # Protobuf 生成代码 (build)
│
├── tests/                          # 测试套件
│   ├── unit/                       # 单元测试
│   │   ├── test_acceptor.cpp
│   │   ├── test_connection.cpp
│   │   ├── test_socket.cpp
│   │   ├── test_packet.cpp
│   │   ├── test_object_pool.cpp
│   │   └── test_env_connection.cpp
│   ├── integration/                # 集成测试
│   │   ├── test_architecture.cpp
│   │   ├── test_sanity.cpp
│   │   └── test_work_stealing_queue.cpp
│   └── benchmark/                  # 性能基准
│       ├── bench_client.cpp        # 压测客户端机器人
│       ├── bench_server.cpp        # 压测服务器端
│       ├── benchmark_log.cpp       # 日志性能
│       ├── benchmark_object_pool.cpp
│       ├── benchmark_packet_pool.cpp
│       ├── benchmark_scheduler.cpp
│       ├── benchmark_robot.cpp     # 全链路机器人压测
│       ├── bm_aoi_grid.cpp         # AOI 性能
│       ├── bm_intrusive_list.cpp
│       ├── bm_packet.cpp
│       ├── bm_spinlock.cpp
│       └── bm_ws_queue.cpp
│
├── vcpkg.json                      # vcpkg 依赖清单
└── docs/                           # 文档 (见下文)
    ├── ARCHITECTURE.md             # 核心架构文档
    ├── ai/                         # AI 友好型文档
    └── interview/                  # 面试准备
```

---

## 协议规范

### 客户端 ↔ GateServer (TCP)

```
[4B Length BigEndian][4B MsgID BigEndian][Protobuf Body]
```

| 字段 | 大小 | 字节序 | 说明 |
|:---|:---|:---|:---|
| Length | 4B | Big Endian | MsgID + Body 总长度 |
| MsgID | 4B | Big Endian | `ids.proto` 中定义的枚举值 |
| Body | Length-4 | Protobuf | 消息体序列化数据 |

### 已定义消息 ID

| ID | 方向 | 名称 | 说明 |
|:---|:---|:---|:---|
| 1001 | CS | LOGIN_REQ | 登录请求 |
| 1002 | SC | LOGIN_RES | 登录响应 |
| 2001 | CS | PING | 心跳 |
| 2002 | SC | PONG | 心跳回复 |
| 2003 | CS | MOVE_REQ | 移动请求 |
| 2004 | SC | MOVE_NTF | 移动通知 (广播) |
| 2005 | SC | ENTER_VIEW | 进入视野 (AOI) |
| 2006 | SC | LEAVE_VIEW | 离开视野 (AOI) |
| 2007 | CS | UPDATE_STATE_REQ | 状态更新 |
| 2008 | SC | STATE_UPDATE_BATCH | 批量状态更新 |
| 2009 | SC | MOVE_NTF_BATCH | 批量移动通知 |
| 2010 | CS | SKILL_CAST_REQ | 技能释放请求 |
| 2011 | SC | DAMAGE_NTF | 伤害通知 |
| 3001 | SS | CREATE_ROOM_REQ | 创建房间 (服务间) |
| 3002 | SS | TERMINATE_ROOM_REQ | 销毁房间 (服务间) |

---

## 测试与基准

```bash
# 单元测试 (CMake CTest)
cd build && ctest --test-dir tests/unit -V

# 集成测试
cd build && ctest --test-dir tests/integration -V

# 全链路压测 (同时绑定 400/600/800 虚拟客户端)
sudo taskset -c 0-3 ./services/gate/gate_server    # Terminal 1
sudo taskset -c 4-7 ./tests/bench_battle           # Terminal 2

# 独立组件基准
./tests/benchmark/bm_aoi_grid          # AOI 性能
./tests/benchmark/benchmark_object_pool # 对象池性能
./tests/benchmark/benchmark_scheduler  # 调度器性能
./tests/benchmark/bm_spinlock          # 自旋锁 vs std::mutex
./tests/benchmark/bm_packet            # 数据包序列化性能
```

---

## 产品化路线图 — Godot 客户端

Aegis Engine 计划通过 Godot 4.x Mono (C#) 客户端实现完整的产品化闭环。

### Phase 1: 基础连通性
- [x] C++ 服务器核心 (Actor + io_uring + AOI)
- [ ] Godot C# TCP 客户端 (Protocol Buffer 序列化)
- [ ] 登录流程 (MsgID 1001/1002)
- [ ] 心跳保活 (MsgID 2001/2002)

### Phase 2: 场景同步
- [ ] 场景加载 + AOI 视野同步
- [ ] 移动同步 (MsgID 2003/2004/2009)
- [ ] 实体进入/离开视野 (MsgID 2005/2006)

### Phase 3: 战斗系统
- [ ] 技能释放 (MsgID 2010)
- [ ] 伤害同步 (MsgID 2011)
- [ ] 状态同步 (MsgID 2007/2008)

### Phase 4: 运营与扩展
- [ ] 房间匹配 (SS 通信)
- [ ] 多场景支持
- [ ] 监控面板 (连接数 / 延迟 / 吞吐)

---

## 开发者

**郝光磊** · 360649459@qq.com · 2026 届校招求职中

> 如果你对高性能游戏服务器、C++20 协程框架或 Linux 内核 IO 技术感兴趣，欢迎交流！
>
> 项目采用 MIT 协议开源。
