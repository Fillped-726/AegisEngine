# AegisEngine Architecture Overview

> AI-oriented: Fast structural understanding of the codebase (updated for directory restructure).

---

## 1. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Application Layer (services/gate)                 │
│  GateServer · PlayerActor · NpcActor · SceneActor · RoomManager     │
│  GameApp · handler_loader · Camp System (create/join/query)          │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ ActorMessage (intrusive MPSC queue)
┌──────────────────────────┴──────────────────────────────────────────┐
│                    Framework Layer (include/aegis/core/)             │
│  Actor (base) · ActorRegistry (ActorID 64-bit) · Task<T>/DetachedTask│
│  Scheduler · Worker (io_uring + coroutine) · mailbox drain          │
│  HierarchicalTimeWheel · SequenceIDGen · RpcManager (coroutine RPC) │
│  Message types (6 files): Net/RPC/Scene/Lifecycle                   │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ co_await / io_uring SQE/CQE
┌──────────────────────────┴──────────────────────────────────────────┐
│                    Network / I/O Layer (include/aegis/net/)          │
│  Socket (RAII) · Acceptor · Connection (shared_ptr)                 │
│  Packet (SBO 1024B) · PacketPool (ObjectPool<Packet, 100000, 128>)  │
│  OutboxBatcher (writev batch, LIMIT=64) · Dispatcher (msg_id→handler)│
│  PacketBuilder (AOI enter/leave serialization)                      │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ io_uring syscalls
┌──────────────────────────┴──────────────────────────────────────────┐
│                    Infrastructure (include/aegis/common/)            │
│  ObjectPool<T,N,M> (TLS+Batch) · IntrusiveList · SpinLock           │
│  WorkStealingQueue · ScopeGuard · UniqueFd · Log (spdlog wrapper)   │
│  TimeUtils · ActorUtils (dispatch_msg helper)                       │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Core Files & Their Roles

### Actor System (`include/aegis/core/`)

| File | Class(es) | Role |
|------|-----------|------|
| `actor.h` | `Actor` | Base class. ActorID, MPSC queue (intrusive), state machine (Active/Idle/Dead), `push()`, `process_batch()` |
| `actor_traits.h` | `SimpleActor<T>`, `PooledActor<T,N,M>` | Two lifecycle strategies: new/delete vs ObjectPool |
| `actor_registry.h` | `ActorRegistry`, `ActorID` | Global singleton, O(1) lock-free `create_actor<T>()`/`get()`/`remove()`. ActorID = 32-bit index + 32-bit version |
| `task.h` | `Task<T>`, `DetachedTask`, `MoveOnlyTask` | C++20 coroutine promise_type. Eager start for DetachedTask, symmetric transfer for Task |
| `worker.h` | `Worker` | Per-thread run loop: io_uring CQEs → coroutine resumption → actor message drain |
| `scheduler.h` | `Scheduler` | Worker pool manager, round-robin startup |
| `hierarchy_timer.h` | `HierarchicalTimeWheel` | 5-level time wheel (256+64×4), O(1) add/cancel/tick, zero-copy cascade via IntrusiveList |
| `sequence_id.h` | `SequenceIDGen` | Per-worker atomic SeqID generator [8-bit worker | 24-bit counter] |

### Message System (`include/aegis/core/message/`)

| File | Contents |
|------|----------|
| `message_base.h` | `ActorMessage` (base) + `BasicMessage<Derived, uint16_t TypeId>` CRTP + `g_message_finalizers[1024]` |
| `message_id.h` | `MessageType : uint16_t` enum (all type_id constants) |
| `message_net.h` | `NetworkMessage` (ObjectPool), `SessionClosedMsg`, `ForwardPacketMsg`, `RebindConnectionMsg` |
| `message_rpc.h` | `RpcMessage<ReqT,ResT,TypeId>`, `RpcResponseMsg` (coroutine) |
| `message_scene.h` | `SceneEnterMsg`, `SceneLeaveMsg`, `SceneMoveMsg`, `SceneSkillCastMsg`, `ReqSceneSnapshotMsg` |
| `message_lifecycle.h` | `CoroutineWakeupMsg`, `ActorDestroyMsg`, `ActorDiedMsg`, `PoisonPillMsg` |

### Game Logic Actors (`include/aegis/game/` — **moved from core/**)

| File | Class(es) | Role |
|------|-----------|------|
| `playerActor.h` | `PlayerActor` | Pooled, owns shared_ptr\<Connection\>, dirty flags (Pos/HP/State), `send_packet`/`send_buffer`, `handle_message()` |
| `scene_actor.h` | `SceneActor` | Pooled, owns AOIGrid + player/NPC maps, OnTick() drives SyncManager, handles Enter/Leave/Move/SkillCast |
| `npc_actor.h` | `NpcActor` | Pooled, owns BT::Tree, AOI-driven AI sleep/wake |
| `room_manager.h` | `RoomManager` | SimpleActor, camp assignment, camp_metas_ cache, player count reporting |
| `sync_manager.h` | `SyncManager` | Dirty-driven template Tick(): AOI move → aggregate batches per receiver → broadcast via callback |
| `aoi_grid.h` | `AOIGrid`, `AOICell` | Grid-based AOI, 9-grid neighbor traversal, SpinLock |
| `ai_nodes.h` | (BT nodes) | BehaviorTree nodes for NPC AI (patrol, chase, attack...) |
| `game_app.h` | `GameApp` | Singleton game bootstrap: creates RoomManager + default main city + NPC spawning |
| `rpc_awaiter.h` | `RpcManager`, `RpcAwaiter<T>` | Coroutine-based async RPC. `co_await RpcCall<ResT>(target, msg)` replaces blocking `future.get()` |

### Network Layer (`include/aegis/net/`)

| File | Class(es) | Role |
|------|-----------|------|
| `socket.h` | `Socket`, `AsyncRead`, `AsyncWrite`, `AsyncReadV`, `AsyncWriteV`, `AsyncAccept` | io_uring async opcodes as C++20 awaitables |
| `acceptor.h` | `Acceptor` | RAII TCP listener, `Task<Socket> accept()` coroutine |
| `connection.h` | `Connection` | Per-peer state machine: `read_packet()` coroutine, `send(PooledPacket)`, `flush()` |
| `packet.h` | `Packet` | SBO-backed 1056B payload container, big-endian wire protocol [SeqID+MsgID+Body] |
| `packetPool.h` | `PacketPool`, `PooledPacket` | ObjectPool\<Packet, 100000, 128\> singleton |
| `outbox_batcher.h` | `OutboxBatcher` | Zero-copy writev view builder, partial write via `advance()`, BATCH_LIMIT=64 |
| `dispatcher.h` | `Dispatcher` | Singleton msg_id→handler router: Net (protobuf-deserializing) and RPC (static_cast) paths |
| `packet_builder.h` | `PacketBuilder` | Stateless factory for AOI EnterView/LeaveView protobuf serialization |

---

## 3. Wire Protocol

```
[4B Length BigEndian][4B SeqID BigEndian][4B MsgID BigEndian][Protobuf Body]
 Length = 8 (SeqID+MsgID) + BodySize
```

- `Packet::pack_into()` writes SeqID + MsgID big-endian + protobuf body
- `OutboxBatcher::prepare_batch()` prepends `htonl(body_len)` before each packet in writev iovec array

---

## 4. Coroutine Execution Model

```
Task<T> (promise_type)
  ├─ initial_suspend → suspend_always (waited → starts)
  ├─ final_suspend → FinalAwaiter: symmetric transfer to continuation
  ├─ return_value / return_void → stores result
  └─ unhandled_exception → stores exception_ptr

DetachedTask (fire-and-forget)
  ├─ initial_suspend → suspend_never (eager start)
  ├─ final_suspend → suspend_never (auto destroy)
  └─ unhandled_exception → log + swallow
```

---

## 5. Threading Model

- N Worker threads (default 4), each with its own io_uring instance
- Each worker: io_uring CQ processing → coroutine resumption → actor message drain
- ActorRegistry::create_actor\<T\>() pins actor to current worker
- Cross-core messages via moodycamel::ConcurrentQueue + eventfd wake
- ActorID versioning prevents ABA across reuse cycles

---

## 6. Key Dependencies

| Library | Usage |
|---------|-------|
| Linux io_uring (liburing) | All async I/O |
| Protobuf 3.x+ | Wire protocol serialization |
| BehaviorTree.CPP 4.x | NPC AI behavior trees |
| moodycamel::ConcurrentQueue | Lock-free queues |
| spdlog | Logging |
| GTest | Unit tests |
| Google Benchmark | Performance benchmarks |

---

## 7. Build System

- CMake + vcpkg (manifest mode)
- Requires: GCC 13+ / Clang 16+, C++20, liburing-dev, Linux 5.10+
- GateServer: `build/services/gate/gate_server`
- Start sequence: `GateServer::init()` → `GameApp::init()` → `GateServer::run(8888)`

---

## 8. Recent Architectural Changes

| Change | Commit | Description |
|--------|--------|-------------|
| game/ from core/ | `3276af8` | Game logic actors moved to `include/aegis/game/`, GateServer no longer depends on game headers |
| RPC coroutinized | `486c3ea` | `RpcCall<ResT>()` replaces blocking `future.get()`, per-worker RpcManager, timeout |
| Message system refactored | `80fe8c2` | 6 files, type_id from uint8→uint16, g_message_finalizers[1024] |
| GateServer slimmed | `f5dfc1c` | Business logic moved to GameApp + handler_loader |
| Camp system (Module C) | `3dcff18+` | RoomManager, dynamic camp create/join/query, player count reporting |
