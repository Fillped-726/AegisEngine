# AegisEngine 文档导航

> C++20 协程 + io_uring Actor 模型游戏服务端框架

---

## 快速导航

| 文档 | 适合谁 | 内容 |
|------|--------|------|
| [**ARCHITECTURE.md**](ARCHITECTURE.md) | **面试官/新读者** | 架构全貌、核心设计、基准数据 |
| [**01-core-framework.md**](01-core-framework.md) | 所有开发者 | Actor 模型、协程、Scheduler/Worker、定时器、序列号 |
| [**02-network-layer.md**](02-network-layer.md) | 网络/通信开发者 | Socket/Connection/Packet/Dispatcher/Acceptor/OutboxBatcher |
| [**03-message-system.md**](03-message-system.md) | 全栈 | 完整消息类型体系、type_id 清单、消息生命周期 |
| [**04-game-logic.md**](04-game-logic.md) | 业务开发者 | PlayerActor/SceneActor/NpcActor/AOIGrid/SyncManager |
| [**05-room-camp.md**](05-room-camp.md) | 业务开发者 | RoomManager/GameApp/营地创建加入查询/RPC 协程 |
| [**06-protobuf.md**](06-protobuf.md) | 全栈 | 全部 proto 文件、MsgID 枚举、CS 协议对照 |
| [**07-build-deploy.md**](07-build-deploy.md) | 部署运维 | CMake/vcpkg/依赖项/启动流程 |
| [**08-design-notes.md**](08-design-notes.md) | 复习与面试 | 关键设计决策与技术选型理由 |
| [**changelog/**](changelog/) | 所有开发者 | 版本更新日志（按日期归档） |
| [**interview/HIGHLIGHTS.md**](interview/HIGHLIGHTS.md) | 面试者 | 10 个可聊亮点 |

## 技术栈

| 层级 | 技术 |
|------|------|
| 语言 | C++20 (服务端) / C# (Godot 客户端) |
| 网络 | io_uring (Linux 5.1+) |
| 并发 | Actor 模型 + 无锁 MPSC 队列 + Thread-per-Core |
| 协程 | 自研 C++20 coroutine (Task/DetachedTask/MoveOnlyTask) |
| 序列化 | Protocol Buffers |
| 构建 | CMake + vcpkg |
| 客户端 | Godot 4.6.2 Mono (C#) |

## 项目结构

```
include/aegis/
├── core/       ← 框架核心（Actor/Task/Scheduler/消息基类/时间轮）
├── game/       ← 游戏业务（PlayerActor/SceneActor/NpcActor/AOI/RoomManager）
├── net/        ← 网络层（Socket/Connection/Packet/Dispatcher/Acceptor）
└── common/     ← 基础工具（ObjectPool/IntrusiveList/SpinLock/UniqueFd）

src/             ← 对应实现
services/gate/   ← 网关服务（GateServer + handler_loader）
shared/proto/    ← Protobuf 定义文件
tests/           ← 单元测试 + 集成测试 + 基准测试
docs/            ← 文档
```

## 数据流

```
[TCP 数据到达]
    │
    ▼ io_uring CQ
Connection::read_packet() 协程恢复
    │
    ▼ 拆包 → PooledPacket
GateServer::dispatch_to_actor()
    │
    ▼ Actor::push() — MPSC 无锁入队
Worker drain → Actor::process_batch()
    │
    ▼ handle_message()
PlayerActor → Dispatcher::dispatch()
    │
    ▼ 业务 handler (协程)
handler(actor, protobuf)
```
