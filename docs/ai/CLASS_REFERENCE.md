# AegisEngine Class Hierarchy Reference

> AI-focused: all public classes, their inheritance, and key methods.

## Actor Hierarchy

```
Actor (base, actor.h)
├── SimpleActor<T> (actor_traits.h)
│   └── RoomManager
├── PooledActor<T, PoolSize, BatchSize> (actor_traits.h)
│   ├── PlayerActor
│   ├── SceneActor
│   └── NpcActor
└── [User extensions]
```

### Actor (actor.h)
```
- id_: ActorID
- worker_id_: int
- msg_queue_: IntrusiveList<ActorMessage>
- virtual handle_message(ActorMessage*) = 0
- virtual finalize() = 0
- base_reset(ActorID, ActorID parent)
- set_worker_id(int)
- enqueue_message(ActorMessage*) - thread-safe
```

### ActorID (actor_registry.h)
```
union { struct { uint32_t index; uint32_t version; }; uint64_t raw; }
- is_valid()
- to_string()
```

## Actor Registry

### ActorRegistry (actor_registry.h)
```
- Singleton: instance()
- template<T> create_actor<T>(Args&&...) → ActorID  (lock-free)
- get(ActorID) → Actor*  (O(1) atomic load)
- remove(ActorID)
- Internal: std::array<atomic<Actor*>, 65536> + moodycamel queue
```

## Network Classes

```
Socket (socket.h)
  └─ RAII UniqueFd wrapper
  └─ Nested awaiters: AsyncRead, AsyncWrite, AsyncReadV, AsyncWriteV, AsyncAccept

Acceptor (acceptor.h)
  └─ Task<Socket> accept()

Connection (connection.h, enable_shared_from_this)
  └─ Task<PooledPacket> read_packet()
  └─ void send(PooledPacket)
  └─ void flush()
  └─ Private: Outbox, OutboxBatcher, rx_buffer_

Packet (packet.h)  [SBO: 1024B stack / heap fallback]
  └─ msg_id() → uint32_t
  └─ pack_into<T>(msg_id, protobuf)
  └─ parse<T>(protobuf&) → bool
  └─ alloc(size), data(), size(), reset()

PacketPool = ObjectPool<Packet, 100000, 128>
PooledPacket = PacketPool::Ptr  (RAII auto-release)

OutboxBatcher (outbox_batcher.h)
  └─ prepare_batch(vector<PooledPacket>) → count
  └─ advance(bytes_written) → packets_completed
  └─ iov_data(), iov_count()
  └─ remaining_iovecs(), is_empty()

Dispatcher (dispatcher.h) [Singleton]
  └─ register_handler<ProtoMsg>(msg_id, func)  [Net, deserializes]
  └─ register_rpc<RpcMsgType>(msg_id, func)    [RPC, static_cast]
  └─ dispatch(actor, packet) → Task<void>
  └─ dispatch_rpc(actor, msg_id, void_ptr) → Task<void>
```

## Coroutine Infrastructure

### Task<T> (task.h)
```
- promise_type (core::TaskPromise<T>)
- co_return → return_value / return_void
- co_await chain support
- WorkStealingQueue-based executor
- DetachedTask: fire-and-forget, runs on current executor
```

## Timer

### HierarchicalTimeWheel (hierarchy_timer.h)
```
- add_timer(delay_ms, callback) → TimerId
- cancel_timer(TimerId)
- tick() - drive by IO thread every TICK_MS(50)
- 5-level cascade: tv1[256] + tv2-5[64] using IntrusiveList
- TimerNodePool (ObjectPool<TimerNode, 100000, 128>)
```

## Game Logic

### PlayerActor (playerActor.h)
```
- PooledActor<PlayerActor>
- Owns shared_ptr<Connection>
- DirtyFlag: DIRTY_POS (1<<0), DIRTY_HP (1<<1), DIRTY_STATE (1<<2)
- MarkDirty/RemoveDirty/IsDirty/ClearDirty/HasAnyDirty
- TakeDamage(int32) → HP→0 triggers DEAD state
- SetPos(x, y, z, dirty_flag)
- GetX()/GetY() (atomic<float>)
- WriteToProto(PBPlayerInfo*, snapshotX, snapshotY)
- send_packet<T>(msg_id, protobuf) - serializes via PacketPool
- send_buffer(msg_id, serialized string) - for SceneActor broadcast
- handle_message: MSG_TYPE_NETWORK → Dispatcher::dispatch() → coroutine
- MSG_TYPE_SESSION_CLOSED → on_session_closed()
- MSG_TYPE_FORWARD_PACKET → send_buffer() [cross-thread broadcast]
```

### SceneActor (scene_actor.h)
```
- PooledActor<SceneActor, 128, 32>
- Owns AOIGrid + unordered_map<ActorID, PlayerActor*>
- AddNpc/RemoveNpc for NpcActor*
- handle_message → OnHandleEnter/Leave/Move/SkillCast
- ProcessAoiEnterLeave(mover, enterIds, leaveIds)
- SendPacket/SendSharedBuffer to specific actor
- OnTick() driven by worker timer
```

### NpcActor (npc_actor.h)
```
- PooledActor<NpcActor, 256, 64>
- Owns BT::Tree (BehaviorTree.CPP v4)
- is_ai_active_ flag for AOI wake/sleep
- OnTick() called by SceneActor
```

### RoomManager (room_manager.h)
```
- SimpleActor<RoomManager>
- Bidirectional map: room_id ↔ actor_id
- on_create_room / on_terminate_room
- on_scene_died(deceased_id, reason) - supervision
```

### SyncManager (sync_manager.h)
```
- dirty_set_ + dirty_players_ (ordered)
- AddDirtyPlayer / RemoveDirtyPlayer
- template Tick(AOIGrid&, onEnterLeave, onSendBatch)
  Phase 1: AOI Move + enter/leave calc
  Phase 2: Aggregate SCMoveNtfBatch per receiver
  Phase 3: onSendBatch callback (SceneActor delivers via ForwardPacketMsg)
  Phase 4: ClearDirty + clear lists
```

### AOIGrid (aoi_grid.h)
```
- Grid-based spatial partitioning
- Add(id, x, y) → gridIndex
- RemoveByGridIndex(id, index) → bool
- Move(id, oldIndex, newX, newY, outEnter, outLeave) → newGridIndex
- GetViewEntityIds(index/xy) → vector
- GetNeighborGridIndices(xy/index) → vector
- ForEachNeighborIndex<T>(centerIndex/xy, visitor) - 9-grid iterator
- SpinLock for thread safety
```

### PacketBuilder (packet_builder.h)
```
- EntityViewInfo{ uid, x, y, entity_type }
- BuildEnterView(EntityViewInfo) → shared_ptr<string>
- BuildEnterView(span<EntityViewInfo>) → shared_ptr<string>  [batch]
- BuildLeaveView(uid) → shared_ptr<string>
- BuildLeaveView(span<uint64_t>) → shared_ptr<string>  [batch]
```

## Message Types (message.h)

| Struct | type_id | Contents |
|--------|---------|----------|
| `BasicMessage<T, type_id>` | auto | CRTP base |
| `NetworkMessage` | MSG_TYPE_NETWORK | `PooledPacket` |
| `SessionClosedMsg` | MSG_TYPE_SESSION_CLOSED | int reason |
| `ForwardPacketMsg` | MSG_TYPE_FORWARD_PACKET | msg_id + shared_buf string |
| `SceneSkillCastMsg` | MSG_TYPE_SCENE_SKILL_CAST | actor_id, skill_id, target_uid, target_x/y |

## Common Utilities (include/aegis/common/)

| File | Class | Purpose |
|------|-------|---------|
| `objectPool.h` | `ObjectPool<T, MaxSize, BatchSize>` | TLS-cached pool, lock-free hot path |
| `intrusive_list.h` | `IntrusiveListNode`, `IntrusiveList<T>` | Zero-overhead linked list |
| `work_stealing_queue.h` | `WorkStealingQueue` | Lock-free work stealing for coroutines |
| `spinLock.h` | `SpinLock` | Busy-wait lock |
| `scopeGuard.h` | `SCOPE_GUARD` macro | RAII scope exit |
| `unique_fd.h` | `UniqueFd` | RAII POSIX fd |
| `time_utils.h` | `TimeUtils` | Timestamp conversion |
| `aegisLog.h` | `Log` (singleton) | Spdlog wrapper, log levels |
| `tools.h` | `Tools` | Misc helpers (fd, time, string) |
| `actor_utils.h` | `ActorUtils` | Helper to send raw_buffer to actor |
