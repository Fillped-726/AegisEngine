# AegisEngine

**自研 C++20 游戏服务端框架** · io_uring + Actor 模型 + 协程运行时

AegisEngine 是一个从零构建的、面向 MMO/ARPG 的游戏服务端框架。核心网络层 (`core` + `net`) 完全自研，在此基础上实现了 Actor 模型运行时、AOI 空间同步、协程 RPC 等游戏服务端通用组件，并通过一个 Godot 联机客户端验证了全链路可用性。

---

## 架构总览

```
┌─────────────────────────────────────────────────────┐
│                业务逻辑层 (game)                      │
│  PlayerActor · SceneActor · NpcActor · 营地系统     │
│  AOI 9宫格 · 脏标记同步 · SyncManager模板化         │
│  战斗系统 · 怪物AI (行为树)                          │
├─────────────────────────────────────────────────────┤
│                服务端运行时 (core)                     │
│  Actor模型(无锁MPSC) · 协程Task<T> · RpcAwaiter     │
│  Thread-per-Core Scheduler · 分层时间轮 · ActorID   │
├─────────────────────────────────────────────────────┤
│                网络基座 (net)                         │
│  io_uring · 协程读写 · 自定义二进制协议              │
│  SBO Packet · PacketPool(10万预分配)                 │
│  OutboxBatcher(writev 64合1) · Connection优雅关闭   │
├─────────────────────────────────────────────────────┤
│              协议层 (shared/proto)                    │
│  Protobuf · 自定义MsgID枚举 · 大端序帧头             │
└─────────────────────────────────────────────────────┘
```

## 核心指标

**基准测试** (Echo 模式, 32B payload, 4 Worker):

| 场景 | QPS | P99 延迟 |
|------|-----|---------|
| 单连接请求-响应 (Depth=1) | 8,116 | 0.24ms |
| 256 连接饱和 (Depth=1) | 303,779 | 2.80ms |
| 流水线模式 (Depth=64, 16 连接) | **2,436,022** | 1.25ms |

**延迟数据** (Depth=1, 32B):
- P50: 0.11ms · P90: 0.15ms · P99: 0.24ms · P99.9: 0.42ms

**吞吐峰值**: 783.9 MB/s (4096B payload, 256 连接)

## 核心特性

| 特性 | 说明 |
|------|------|
| **自研 Actor 模型** | 无锁 MPSC 消息队列 + 64-bit 版本号句柄防 ABA + 缓存行对齐 |
| **io_uring + C++20 协程** | 所有 IO 操作为 awaitable，线性代码替代回调嵌套 |
| **Thread-per-Core** | 每个 Worker 独立 io_uring 实例，无锁跨核通信 (moodycamel + eventfd) |
| **分层时间轮** | 5 级时间轮，O(1) 添加/取消，侵入式链表零拷贝级联 |
| **零拷贝网络栈** | SBO 1024B 栈内分配 + ObjectPool + writev 批量发送 (64 包/次 syscall) |
| **AOI 空间同步** | 9 宫格网格 + 脏标记驱动增量同步，模板化 SyncManager 解耦算法与发送 |
| **协程 RPC** | `co_await RpcCall<T>(target, msg)` 非阻塞跨 Worker 调用 |
| **优雅关闭** | Connection::close() 带 shutdown + 防重复关闭 + 发送队列背压 |

## 快速开始

```bash
# 依赖: cmake, vcpkg, C++20 编译器

# 构建 (build 目录已配置好)
cd AegisEngine
cmake -B build
cmake --build build -j$(nproc)

# 启动 echo 服务端
./build/tests/bm_echo

# 新终端: 运行基准测试
./build/tests/bench_client

# 启动完整游戏服务端
./build/bin/gate_server

# 启动 Godot 客户端 (Windows)
# 打开 C:\D\project\aegis-client 项目, 运行 CampScene.tscn
```

## 文档导航

| 文档 | 适合谁 |
|------|--------|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 想了解架构全貌的人 |
| [docs/01-core-framework.md](docs/01-core-framework.md) | Actor / 协程 / 调度器 / 定时器 |
| [docs/02-network-layer.md](docs/02-network-layer.md) | 网络层 (Socket/Connection/Packet/Dispatcher) |
| [docs/03-message-system.md](docs/03-message-system.md) | 消息体系与生命周期 |
| [docs/04-game-logic.md](docs/04-game-logic.md) | 游戏逻辑 (AOI/SyncManager/NpcActor) |
| [docs/05-room-camp.md](docs/05-room-camp.md) | 营地系统 (RoomManager/GameApp/RPC) |
| [docs/08-design-notes.md](docs/08-design-notes.md) | 设计决策与踩坑记录 |
| [docs/changelog/](docs/changelog/) | 版本更新日志 |
| [docs/interview/HIGHLIGHTS.md](docs/interview/HIGHLIGHTS.md) | 面试亮点 10 讲 |

## 项目背景

本项目始于个人对 C++ 服务端技术的实践追求。核心网络层从零构建（不使用任何第三方网络库），旨在深入理解：

- **io_uring** 与异步 IO 的编程模型
- **Actor 模型**在游戏服务端中的落地
- **C++20 协程**在真实项目中的工程化应用
- **无锁数据结构**与高性能内存管理

在完成框架基座后，通过一个 Godot C# 联机客户端验证了全链路功能，包括 AOI 空间同步、脏标记驱动的增量状态广播、战斗系统等。

## License

MIT
