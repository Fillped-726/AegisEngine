# AegisEngine Architecture Overview

> AI-oriented: Fast structural understanding of the codebase.

## 1. High-Level Architecture

```
┌─────────────────────────────────────────────────────┐
│                    Application Layer                 │
│  PlayerActor  NpcActor  SceneActor  RoomManager      │
│     (pooled)    (pooled)   (pooled)  (simple)        │
└──────────────────────┬──────────────────────────────┘
                       │ ActorMessage (intrusive list)
┌──────────────────────┴──────────────────────────────┐
│                    Actor Layer                        │
│  Actor (base) → ActorRegistry → ActorID 64-bit      │
│  ActorMessage queue (work-stealing)                  │
│  Scheduler + Worker pool                             │
└──────────────────────┬──────────────────────────────┘
                       │ io_uring CQEs / coroutine resumption
┌──────────────────────┴──────────────────────────────┐
│                    I/O Layer                          │
│  Acceptor → Connection → Socket → io_uring          │
│  Packet (SBO) + PacketPool + OutboxBatcher (writev) │
│  Dispatcher (msg_id → handler map)                  │
└─────────────────────────────────────────────────────┘
```

## 2. Core Files & Their Roles

### Actor System (`include/aegis/core/`)

| File | Class(es) | Role |
|------|-----------|------|
| `actor.h` | `Actor` | Base class for all actors. Has `ActorID id_`, `ActorMessage` queue (intrusive list), `set_worker_id()`, `base_reset()`, `finalize()` |
| `actor_traits.h` | `SimpleActor<T>`, `PooledActor<T,N,M>` | Two lifecycle strategies: `SimpleActor` uses new/delete, `PooledActor` uses `ObjectPool<T,N,M>` |
| `actor_registry.h` | `ActorRegistry`, `ActorID` | Global singleton, O(1) lock-free `create_actor<T>()`/`get()`/`remove()`. `ActorID` = 32-bit index + 32-bit version (ABA prevention) |
| `message.h` | `ActorMessage`, `NetworkMessage`, `BasicMessage<Derived,TypeId>`, `ForwardPacketMsg`, `SessionClosedMsg` | Intrusive linked-list messages; `type_id` for dispatch; `NetworkMessage` wraps `PooledPacket` |
| `task.h` | `Task<T>`, `DetachedTask` | C++20 coroutine promise_type + `WorkStealingQueue`-based executor (see below) |
| `worker.h` | `Worker` | Thread-local run loop: polls io_uring CQEs, coroutine resumption, and actor message draining |
| `scheduler.h` | `Scheduler` | `Worker` pool manager, round-robin startup |

### Timer System (`include/aegis/core/hierarchy_timer.h`)

| Class | Role |
|-------|------|
| `TimerNode` | Intrusive list node + `expires` tick + `TimerCallback` |
| `HierarchicalTimeWheel` | 5-level time wheel (256 + 64×4), O(1) add/cancel/tick, zero-copy cascade via `IntrusiveList` |

### Game Logic Actors (`include/aegis/core/`)

| File | Class(es) | Role |
|------|-----------|------|
| `playerActor.h` | `PlayerActor` | Pooled, owns `Connection` shared_ptr, sets dirty flags (Pos/HP/State), `WriteToProto()`, `send_packet()`/`send_buffer()`, `handle_message()` dispatches `MSG_TYPE_NETWORK` to `Dispatcher::dispatch()` |
| `scene_actor.h` | `SceneActor` | Manages AOI grid + player/NPC maps, handles Enter/Leave/Move/SkillCast, drives `SyncManager::Tick()` |
| `npc_actor.h` | `NpcActor` | Pooled, owns BehaviorTree (`BT::Tree`), driven by SceneActor tick |
| `room_manager.h` | `RoomManager` | SimpleActor, bidirectional room↔scene mapping, create/terminate room logic |
| `sync_manager.h` | `SyncManager` | Dirty player tracking + AOI Move + batch broadcast; template Tick() with 4 phases: calc enter/leave → aggregate SCMoveNtfBatch → serialize once per receiver → clear dirty |
| `ai_nodes.h` | (BehaviorTree nodes) | BT nodes for NPC AI (patrol, chase, attack, etc.) |
| `GameMessage.h` | `SceneSkillCastMsg` | Example game message struct with `BasicMessage<T, TypeId>` pattern |
| `aoi_grid.h` | `AOIGrid`, `AOICell` | Grid-based AOI: `Add()`/`RemoveByGridIndex()`/`Move()` with enter/leave vectors, 9-grid neighbor traversal via `ForEachNeighborIndex()`, thread-safe via `SpinLock` |

### Network Layer (`include/aegis/net/`)

| File | Class(es) | Role |
|------|-----------|------|
| `socket.h` | `Socket`, `AsyncRead`, `AsyncWrite`, `AsyncReadV`, `AsyncWriteV`, `AsyncAccept` | io_uring async opcodes as C++20 awaitables, SQE/CQE lifecycle |
| `acceptor.h` | `Acceptor` | RAII TCP listener, `Task<Socket> accept()` coroutine |
| `connection.h` | `Connection` | Per-peer state machine: `read_packet()` coroutine, `send(PooledPacket)`, `flush()` |
| `packet.h` | `Packet` | SBO-backed 1056B payload container, `pack_into()` (serialize header + protobuf), `parse<T>()` (deserialize), big-endian wire format |
| `packetPool.h` | `PacketPool` (alias `ObjectPool<Packet,100000,128>`), `PooledPacket` | Singleton pooled packet allocator |
| `outbox_batcher.h` | `OutboxBatcher` | Zero-copy writev view builder, partial write support via `advance()`, BATCH_LIMIT=64 |
| `dispatcher.h` | `Dispatcher` | Singleton msg_id → handler router for Net (protobuf-deserializing) and RPC (static_cast) paths |
| `packet_builder.h` | `PacketBuilder` | Stateless factory for AOI EnterView/LeaveView protobuf serialization |

## 3. Wire Protocol

```
[4B Length (Big Endian)][4B MsgID (Big Endian)][Protobuf Body]
 Length = MSGID_SIZE(4) + BodySize
```

- `Packet::pack_into()` writes the header manually (big-endian via `htonl` or `__builtin_bswap32`)
- `OutboxBatcher::prepare_batch()` prepends `htonl(body_len)` before each packet in the iovec array (so on-the-wire format is `[4B len][4B msg_id][body]`)

## 4. Coroutine Execution Model

```
Task<T> (promise_type)
  ├─ initial_suspend → suspend_never (eager start)
  ├─ final_suspend → resume caller if not detached
  ├─ yield_value → suspend, resume worker
  └─ await_transform
       ├─ Task<T> → chain (await child task on same executor)
       ├─ WorkerAwaiter → suspend, enqueue to WorkStealingQueue
       ├─ Socket::AsyncRead/Accept/Write → suspend, submit io_uring SQE
       └─ SleepAwaiter → suspend, register with HierarchicalTimeWheel
```

Key design: task execution is **eager** (starts immediately in the caller's context) and **work-stealing** (awaited tasks can migrate between worker threads).

## 5. Threading Model

- N worker threads (configurable)
- Each worker: `io_uring` CQ processing → coroutine resumption → actor message draining
- `ActorRegistry::create_actor<T>()` pins actor to current worker via `set_worker_id()`
- Messages between workers: inter-worker message queue
- `ActorRegistry::get(ActorID)` is lock-free O(1) via `std::atomic<Actor*>` array

## 6. Key Dependencies

| Library | Version | Usage |
|---------|---------|-------|
| Linux io_uring | liburing | All async I/O (accept/read/write) |
| Protobuf | 3.x+ | Wire protocol serialization |
| BehaviorTree.CPP | 4.x | NPC AI behavior trees |
| moodycamel::ConcurrentQueue | header-only | ActorRegistry free-index queue |
| Google Benchmark | (test) | Performance benchmarks |
| GTest | (test) | Unit tests |

## 7. Build System

- CMake + vcpkg (manifest mode, `vcpkg.json`)
- Requires: GCC 13+ or Clang 16+, C++20, liburing-dev
- Tests: `ctest` or individual test binaries
- Only Linux (no Windows/macOS due to io_uring dependency)
