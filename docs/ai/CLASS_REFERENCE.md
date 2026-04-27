# AegisEngine Class Hierarchy Reference

> AI-focused: all public classes, their inheritance, and key methods (updated for game/ directory).

---

## Actor Hierarchy

```
Actor (base, actor.h)
├── SimpleActor<T> (actor_traits.h)       — new/delete lifecycle
│   └── RoomManager
├── PooledActor<T, PoolSize, BatchSize>    — ObjectPool lifecycle
│   ├── PlayerActor
│   ├── SceneActor
│   └── NpcActor
└── [User extensions]
```

## Actor (actor.h)

```
- id_: ActorID
- parent_id_: ActorID (supervision tree)
- worker_id_: int
- tail_: atomic<ActorMessage*> (MPSC queue tail)
- head_: ActorMessage* (consumer local)
- is_scheduled_: atomic<bool> (state machine)
- ActorState: Active / Idle / Dead

- push<T>(msg) -> bool          — thread-safe MPSC enqueue
- process_batch(budget) -> state  — drain messages
- handle_message(msg) = 0       — subclass implements
- finalize() = 0                — subclass implements
- schedule_timer(delay_ms, cb)  — register with time wheel
- base_reset(new_id, new_parent)
```

## ActorID (actor_registry.h)

```
union { struct { uint32_t index; uint32_t version; }; uint64_t raw; }
  - is_valid()
  - to_string()
```

## ActorRegistry (actor_registry.h)

```
- Singleton: instance()
- template<T> create_actor<T>(Args&&...) -> ActorID (lock-free)
- get(ActorID) -> Actor* (O(1) atomic load)
- remove(ActorID)
- Internal: array<atomic<Actor*>, 65536> + moodycamel queue
```

---

## Network Classes

```
Socket (socket.h) — RAII UniqueFd wrapper
  └─ Nested awaiters: AsyncRead, AsyncWrite, AsyncReadV, AsyncWriteV, AsyncAccept

Acceptor (acceptor.h)
  └─ Task<Socket> accept()

Connection (connection.h, enable_shared_from_this)
  └─ Task<PooledPacket> read_packet()    — coroutine, TCP stream deframing
  └─ void send(PooledPacket)             — enqueue to outbox
  └─ void flush()                        — trigger send_batch_coro (writev)
  └─ Private: Outbox, OutboxBatcher, rx_buffer_

Packet (packet.h) [SBO: 1024B stack / heap fallback]
  └─ msg_id() -> uint32_t                — read big-endian MsgID
  └─ seq_id() -> uint32_t                — read big-endian SeqID
  └─ pack_into<T>(msg_id, seq_id, protobuf)  — serialize
  └─ parse<T>(protobuf&) -> bool         — deserialize
  └─ alloc(size), data(), size(), reset()

PacketPool = ObjectPool<Packet, 100000, 128>
PooledPacket = PacketPool::Ptr (RAII auto-release)

OutboxBatcher (outbox_batcher.h)
  └─ prepare_batch(vector<PooledPacket>) -> count
  └─ advance(bytes_written) -> packets_completed
  └─ iov_data(), iov_count()
  └─ BATCH_LIMIT = 64

Dispatcher (dispatcher.h) [Singleton]
  └─ register_handler<ProtoMsg>(msg_id, func)  [Net: deserializes protobuf]
  └─ register_rpc<RpcMsgType>(msg_id, func)    [RPC: static_cast]
  └─ dispatch(actor, packet) -> Task<void>     [Net path]
  └─ dispatch_rpc(actor, msg_id, ptr) -> Task<void> [RPC path]
```

---

## Coroutine Infrastructure

### Task<T> (task.h)

```
- promise_type (Promise<T> / Promise<void> extends PromiseBase)
- co_return -> return_value / return_void
- co_await chain via continuation_ + symmetric transfer
- initial_suspend: suspend_always (waited before starting)
- DetachedTask: fire-and-forget, eager, auto-destroy
- MoveOnlyTask: type-erased functor wrapper
```

---

## Timer

### HierarchicalTimeWheel (hierarchy_timer.h)

```
- add_timer(delay_ms, callback) -> TimerId    — O(1)
- cancel_timer(TimerId)                        — O(1)
- tick() — drive by Worker every TICK_MS(50)
- 5-level cascade: tv1[256] + tv2-5[64] using IntrusiveList
- Zero-copy cascade via splice
```

---

## Game Logic (in aegis::core:: namespace, under include/aegis/game/)

### PlayerActor (playerActor.h)

```
- PooledActor<PlayerActor>
- Owns shared_ptr<Connection>
- DirtyFlag: DIRTY_POS(1<<0), DIRTY_HP(1<<1), DIRTY_STATE(1<<2)
- Atomic coords: x_, y_ (atomic<float>)
- MarkDirty/RemoveDirty/IsDirty/ClearDirty/HasAnyDirty
- TakeDamage(int32) -> HP→0 → DEAD state
- SetPos(x, y, z, dirty_flag)
- WriteToProto(PBPlayerInfo*, snapshotX, snapshotY)
- send_packet<T>(msg_id, seq_id, protobuf) — serializes via PacketPool
- send_buffer(msg_id, seq_id, string) — pre-serialized (broadcast opt)
- handle_message: MSG_TYPE_NETWORK → Dispatcher::dispatch()
                  MSG_TYPE_SESSION_CLOSED → on_session_closed()
                  MSG_TYPE_FORWARD_PACKET → send_buffer()
                  MSG_TYPE_RPC_RESPONSE → RpcManager::consume()
```

### SceneActor (scene_actor.h)

```
- PooledActor<SceneActor, 128, 32>
- Owns AOIGrid + unordered_map<ActorID, PlayerActor*> + unordered_map<raw_id, NpcActor*>
- handle_message → OnHandleEnter/Leave/Move/SkillCast
- OnTick() — 50ms timer: sync_mgr_.Tick() + NPC AI + player count report
- ProcessAoiEnterLeave(mover, enterIds, leaveIds)
- AddNpc / RemoveNpc
- SendSharedBuffer(targetId, msgId, shared_ptr<string>)
```

### NpcActor (npc_actor.h)

```
- PooledActor<NpcActor, 256, 64>
- Owns BT::Tree (BehaviorTree.CPP v4)
- is_ai_active_ flag — AOI-driven sleep/wake
- OnTick() — behavior tree tick (returns immediately if !active)
- SetScene(SceneActor*) — for BT node query callback
```

### RoomManager (room_manager.h)

```
- SimpleActor<RoomManager>
- camp_metas_ map: scene_actor_id → CampMeta { name, current_players, max_players }
- on_assign_camp(RPCAssignCampMsg) — create or join camp
- on_camp_player_count(CampPlayerCountMsg) — periodic player count update
- destroying_scenes_ set — for supervision
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
- Add(id, x, y) -> gridIndex
- RemoveByGridIndex(id, index) -> bool
- Move(id, oldIndex, newX, newY, outEnter, outLeave) -> newGridIndex
- GetViewEntityIds(index/xy) -> vector (9-grid neighbors)
- SpinLock for thread safety
```

### PacketBuilder (packet_builder.h)

```
- EntityViewInfo { uid, x, y, entity_type }
- BuildEnterView(EntityViewInfo) -> shared_ptr<string>
- BuildEnterView(span<EntityViewInfo>) -> shared_ptr<string>
- BuildLeaveView(uid) -> shared_ptr<string>
- BuildLeaveView(span<uint64_t>) -> shared_ptr<string>
```

### GameApp (game_app.h)

```
- Singleton game bootstrap
- init(width, height, cellSize, workerId) -> creates RoomManager + main city scene
- default_scene_id() -> ActorID
- room_manager_id() -> ActorID
```

### RpcAwaiter / RpcManager (rpc_awaiter.h)

```
- RpcManager (thread_local singleton)
  └─ register_pending(rpc_id, handle, slot, timer_id)
  └─ consume(rpc_id, result_ptr) -> coroutine_handle<>
  └─ on_timeout(rpc_id)
  └─ next_id() -> RpcId [16-bit worker | 48-bit counter]

- RpcAwaiter<ResT>
  └─ await_suspend: generate rpc_id, register pending, dispatch_msg
  └─ await_resume: throw on timeout, return result

- RpcCall<ResT>(target, msg, timeout_ms) -> RpcAwaiter<ResT>
```

---

## Message Types

| Struct | type_id | Contents |
|--------|---------|----------|
| `BasicMessage<T, uint16_t TypeId>` | auto | CRTP base |
| `NetworkMessage` | MSG_TYPE_NETWORK | PooledPacket |
| `SessionClosedMsg` | MSG_TYPE_SESSION_CLOSED | int reason |
| `ForwardPacketMsg` | MSG_TYPE_FORWARD_PACKET | msg_id + seq_id + shared_buf |
| `RebindConnectionMsg` | MSG_TYPE_REBIND_CONNECTION | shared_ptr<Connection> |
| `RpcResponseMsg` | MSG_TYPE_RPC_RESPONSE | rpc_id + result_storage |
| `SceneEnterMsg` | MSG_TYPE_SCENE_ENTER | actor_id, player_id, x, y |
| `SceneLeaveMsg` | MSG_TYPE_SCENE_LEAVE | actor_id, player_id |
| `SceneMoveMsg` | MSG_TYPE_SCENE_MOVE | actor_id, player_id, grid_index, newX, newY, dir |
| `SceneSkillCastMsg` | MSG_TYPE_SCENE_SKILL_CAST | actor_id, skill_id, target_uid, target_x, target_y |
| `ReqSceneSnapshotMsg` | MSG_TYPE_REQ_SCENE_SNAPSHOT | player_id |
| `CampPlayerCountMsg` | MSG_TYPE_CAMP_PLAYER_COUNT | scene_actor_id, count |
| `CoroutineWakeupMsg` | MSG_TYPE_CORO_WAKEUP | coroutine_handle |
| `ActorDestroyMsg` | MSG_TYPE_DESTROY | (empty) |
| `ActorDiedMsg` | MSG_TYPE_ACTOR_DIED | deceased_id, reason |
| `PoisonPillMsg` | MSG_TYPE_POISON_PILL | (empty) |

---

## Common Utilities (include/aegis/common/)

| File | Class | Purpose |
|------|-------|---------|
| `objectPool.h` | `ObjectPool<T, MaxSize, BatchSize>` | TLS-cached pool, lock-free hot path |
| `intrusive_list.h` | `IntrusiveListNode`, `IntrusiveList<T>` | Zero-overhead linked list |
| `work_stealing_queue.h` | `WorkStealingQueue` | Lock-free work stealing |
| `spinLock.h` | `SpinLock` | Busy-wait lock |
| `scopeGuard.h` | `SCOPE_GUARD` macro | RAII scope exit |
| `unique_fd.h` | `UniqueFd` | RAII POSIX fd |
| `time_utils.h` | `TimeUtils` | Timestamp conversion |
| `aegisLog.h` | `Log` (singleton) | Spdlog wrapper |
| `tools.h` | `Tools` | Misc helpers |
| `actor_utils.h` | `ActorUtils`, `dispatch_msg()` | Cross-worker actor message forwarding |
