# AegisEngine 文档导航

> C++20 协程 + io_uring Actor 模型游戏服务端框架
> 最后更新: 2026-04-27

---

## 快速导航

| 文档 | 适合谁 | 内容 |
|------|--------|------|
| [**01-core-framework.md**](01-core-framework.md) | 所有开发者 | Actor 模型、协程、Scheduler/Worker、定时器、序列号 |
| [**02-network-layer.md**](02-network-layer.md) | 网络/通信开发者 | Socket/Connection/Packet/Dispatcher/Acceptor/OutboxBatcher |
| [**03-message-system.md**](03-message-system.md) | 全栈 | 完整消息类型体系、type_id 清单、消息生命周期 |
| [**04-game-logic.md**](04-game-logic.md) | 业务开发者 | PlayerActor/SceneActor/NpcActor/AOIGrid/SyncManager |
| [**05-room-camp.md**](05-room-camp.md) | 业务开发者 | RoomManager/GameApp/营地创建加入查询/RPC 协程 |
| [**06-protobuf.md**](06-protobuf.md) | 全栈 | 全部 proto 文件、MsgID 枚举、CS 协议对照 |
| [**07-build-deploy.md**](07-build-deploy.md) | 部署运维 | CMake/vcpkg/依赖项/启动流程 |
| [**08-design-notes.md**](08-design-notes.md) | 复习与面试 | 关键设计决策与技术选型理由 |
| [**ai/ARCHITECTURE.md**](ai/ARCHITECTURE.md) | AI Agent | 精简架构速览 |
| [**ai/CLASS_REFERENCE.md**](ai/CLASS_REFERENCE.md) | AI Agent | 类继承与方法签名 |
| [**ai/DATA_FLOWS.md**](ai/DATA_FLOWS.md) | AI Agent | 核心数据流 |
| [**ai/QUICK_START.md**](ai/QUICK_START.md) | AI Agent | 项目第一入口 |
| [**interview/HIGHLIGHTS.md**](interview/HIGHLIGHTS.md) | 面试者 | 10 个可聊亮点 |
| [**interview/INTERVIEW_PREP.md**](interview/INTERVIEW_PREP.md) | 面试者 | 面试问答深度解析 |

---

## 架构全景

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Application Layer (services/gate)                 │
│  GateServer · PlayerActor · NpcActor · SceneActor · RoomManager     │
│  GameApp · handler_loader · 营地创建/加入/查询                      │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ ActorMessage (Intrusive MPSC queue)
┌──────────────────────────┴──────────────────────────────────────────┐
│                    Framework Layer (include/aegis/core/)             │
│  Actor base · ActorRegistry (ActorID 64-bit) · Task<T> · DetachedTask│
│  Scheduler · Worker (io_uring + coroutine) · mailbox drain          │
│  HierarchicalTimeWheel · SequenceIDGen · RpcManager (coroutine RPC) │
│  Message types: Net/RPC/Scene/Lifecycle                             │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ co_await / io_uring SQE/CQE
┌──────────────────────────┴──────────────────────────────────────────┐
│                    Network / I/O Layer (include/aegis/net/)          │
│  Socket (RAII) · Acceptor · Connection (shared_ptr)                 │
│  Packet (SBO 1024B) · PacketPool (ObjectPool)                       │
│  OutboxBatcher (writev batch) · Dispatcher (msg_id → handler)       │
│  PacketBuilder (AOI enter/leave serialization)                      │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ io_uring syscalls
┌──────────────────────────┴──────────────────────────────────────────┐
│                    Infrastructure (include/aegis/common/)            │
│  ObjectPool<T,N,M> (TLS+Batch) · IntrusiveList · SpinLock           │
│  WorkStealingQueue · ScopeGuard · UniqueFd · Log (spdlog wrapper)   │
│  TimeUtils · ActorUtils                                             │
└─────────────────────────────────────────────────────────────────────┘
```

## 目录结构

```
AegisEngine/
├── include/aegis/          # 所有头文件 (Public API)
│   ├── common/             # 基础设施：ObjectPool / IntrusiveList / Log / SpinLock
│   ├── core/               # 框架核心：Actor / Task / Scheduler / Worker / 消息系统
│   └── game/               # 游戏业务：PlayerActor / SceneActor / NpcActor / AOI
│   └── net/                # 网络层：Socket / Connection / Packet / Dispatcher
│
├── src/                    # 实现文件
│   ├── common/             # 通用工具实现
│   ├── core/               # 框架核心实现
│   ├── game/               # 游戏逻辑实现
│   └── net/                # 网络层实现
│
├── services/               # 可执行服务
│   ├── gate/               # GateServer — TCP 网关
│   └── logic/              # LogicServer (预留)
│
├── shared/                 # 跨服务共享
│   └── proto/              # Protobuf 定义
│
├── tests/                  # 测试
│   ├── unit/               # 单元测试 (GTest)
│   ├── integration/        # 集成测试
│   └── benchmark/          # 性能基准 (Google Benchmark)
│
├── docs/                   # 文档 (你正在看的地方)
├── cmake/                  # CMake 模块
├── vcpkg_installed/        # vcpkg 声明的依赖
└── CMakeLists.txt          # 顶层构建文件
```

> 💡 **给 AI Agent 的提示**: 先看 [`ai/QUICK_START.md`](ai/QUICK_START.md) 获取项目鸟瞰和 AI 阅读指南，然后按需深入到各模块文档。
