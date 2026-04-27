# AegisEngine — AI Quick Start Guide

> 如果你是 AI Agent，从这个文件开始阅读。它给你项目鸟瞰和阅读路径。

---

## 一句话

C++20 协程 + io_uring + Actor 模型的 MMO 游戏服务端框架。每个玩家/场景/NPC 都是一个 Actor，有自己的 Worker 绑定和消息队列。

## 快速导航

| 你想做什么 | 看这个文件 |
|-----------|----------|
| 理解整体分层 | `01-core-framework.md` (框架) + `02-network-layer.md` (网络) |
| 排查消息处理问题 | `03-message-system.md` (消息类型) + `04-game-logic.md` (PlayerActor) |
| 了解营地和房间 | `05-room-camp.md` |
| 了解协议和 MsgID | `06-protobuf.md` |
| 了解构建和部署 | `07-build-deploy.md` |
| 理解设计理由 | `08-design-notes.md` |

## 关键路径速查

```
TCP → read_packet() → Actor::push() → handle_message() → dispatcher.dispatch()
                 connection.h         actor.h           playerActor.h     dispatcher.h

GateServer::accept_loop() → GateServer::handle_session()
         gate_server.cpp              gate_server.cpp
```

## 重要文件一览

**框架核心**:
- `actor.h` — Actor 基类 + MPSC 邮箱
- `task.h` — Task\<T\> / DetachedTask 协程类型
- `worker.h` — Worker: io_uring + 协程 + actor drain
- `scheduler.h` — Worker 池管理器
- `hierarchy_timer.h` — 5 级时间轮

**游戏逻辑** (在 `game/` 下):
- `playerActor.h` — 玩家 Actor
- `scene_actor.h` — 场景 Actor (AOI + Tick)
- `npc_actor.h` — NPC Actor (行为树)
- `room_manager.h` — 营地/房间管理

**网络**:
- `connection.h` — TCP 连接拆包/发送
- `packet.h` — SBO 网络包
- `dispatcher.h` — msg_id → handler 路由器

## 注意 (给 AI)

1. `include/aegis/core/message/message_base.h` — `type_id` 现在是 **uint16_t**
2. `include/aegis/game/` — 游戏逻辑已经从 `core/` 迁移到了这里
3. 消息的 `finalize()` 通过 `g_message_finalizers[type_id]` 静态注册表调用，不是虚函数
