# AegisEngine Data Flow Diagrams

> Protocol flow, cross-thread messaging, and hot paths for AI code understanding (updated for RPC coroutine + camp system).

---

## Flow 1: TCP Connection → Packet → Handler

```
Client                          Acceptor (W0)           Worker (0~3)
  │                                │                        │
  │  ──TCP SYN─────────────────►   │                        │
  │                                │  accept() coroutine    │
  │                                │  → Socket(accepted_fd) │
  │                                │  → Connection(socket)  │
  │                                │  → post_custom_task →  │
  │                                │      target_worker     │
  │                                │                        │
  │  ──[4B len][4B seq][4B msg_id][body] ──────────────►   │
  │                                │                        │
  │                                │  io_uring CQE          │
  │                                │  → Connection::read_packet() resumes
  │                                │  → Assembled PooledPacket
  │                                │  → Actor::push(NetworkMessage)
  │                                │                        │
  │                                │[Worker drain loop]     │
  │                                │  → PlayerActor::handle_message()
  │                                │    → MSG_TYPE_NETWORK  │
  │                                │    → Dispatcher::dispatch(actor, pkt)
  │                                │      → handler(actor, parsed_proto)
  │                                │        → co_await business_logic
  │                                │                        │
  │  ◄──[4B len][4B seq][4B msg_id][body] ──────────────── │
  │        (response via Connection::send())               │
```

**Stage locations**:
| Stage | File | Function |
|-------|------|----------|
| accept | `gate_server.cpp` | `GateServer::accept_loop()` |
| session | `gate_server.cpp` | `GateServer::handle_session()` |
| read_packet | `connection.cpp` | `Connection::read_packet()` |
| dispatch to actor | `gate_server.cpp` | `GateServer::dispatch_to_actor()` |
| Actor push | `actor.h` | `Actor::push()` |
| handle_message | `playerActor.h` | `PlayerActor::handle_message()` |
| Dispatcher | `dispatcher.h` | `Dispatcher::dispatch()` |
| Handler | `handler_loader.cpp` | registered lambda |

---

## Flow 2: State Sync (SyncManager Tick)

```
Tick (50ms timer on SceneActor's worker)

  ┌──────────────────────────────────────────────┐
  │ SyncManager::Tick(aoi, enterLeaveCb, sendCb)  │
  └──────────────────────────────────────────────┘
         │
         ▼
  Phase 1: For each dirty player with DIRTY_POS
  ─────────────────────────────────────────────────
  AOI::Move(id, oldGrid, newX, newY, enterIds, leaveIds)
      ↓
  grid changed? → enterLeaveCb(mover, enterIds, leaveIds)
    → SceneActor::ProcessAoiEnterLeave()
      → PacketBuilder::BuildEnterView() / BuildLeaveView()
      → SendSharedBuffer() via ForwardPacketMsg
         │
         ▼
  Phase 2: For each dirty player
  ─────────────────────────────────────────────────
  AOI::GetViewEntityIds(mover.grid, neighbors)
    For each neighbor != self:
      → receiver_batches[neighbor].add_move(mover.info)
         │
         ▼
  Phase 3: For each (receiverActorID, batch)
  ─────────────────────────────────────────────────
  shared_buf = make_shared<string>(batch.SerializeAsString())
  → onSendBatch(receiverActorId, shared_buf)
    → SceneActor::SendSharedBuffer()
      → ForwardPacketMsg(msg_id, seq_id, shared_buf)
      → dispatch_msg → PlayerActor
         │
         ▼
  Phase 4: ClearDirty for all processed players
         │
         ▼
  PlayerActor receives ForwardPacketMsg
    → handle_message(MSG_TYPE_FORWARD_PACKET)
      → send_buffer(msg_id, seq_id, *shared_buf)
        → Connection::send(PooledPacket)
          → Outbox + flush → writev via io_uring
```

---

## Flow 3: Cross-Worker Message Delivery

```
Worker A (SceneActor)             Worker B (PlayerActor)
      │                                │
      │  dispatch_msg(                 │
      │    ForwardPacketMsg)            │
      │  ──────────────────────────►   │
      │                                │ Worker B polls its msg queue
      │                                │ → PlayerActor::handle_message()
      │                                │ → send_buffer() → socket write
```

Key: `Actor::push()` is thread-safe (lock-free MPSC). `dispatch_msg()` handles intra/inter-worker routing.

---

## Flow 4: Coroutine Lifecycle

```
Caller:
  auto task = some_coroutine();
  // initial_suspend = suspend_always → not started yet

  co_await task;
  // If not ready → suspend caller, chain callee's completion to caller

Task body:
  co_await socket.recv(buf, len);
  // → suspend, submit io_uring SQE
  // → Worker polls CQ, resumes coroutine in Worker thread

  co_await worker.yield();
  // → suspend, push to WorkStealingQueue
  // → Any worker steals and resumes

  co_await sleep(100ms);
  // → suspend, register with HierarchicalTimeWheel
  // → After 100ms, timer tick enqueues resumption via WorkStealingQueue

DetachedTask:
  // No co_await needed — initial_suspend = suspend_never
  // Created in GateServer::handle_session() and background tasks
  // Auto-destroyed on completion, exceptions logged
```

---

## Flow 5: AOI Grid Spatial Query

```
World: 1000x1000, cellSize=100 → 10x10 = 100 cells

Player at (350, 250):
  col = 350/100 = 3, row = 250/100 = 2
  centerGridIndex = 2*10 + 3 = 23

9-grid neighbors (col 2-4, row 1-3):
  [12][13][14]
  [22][23][24]  ← center
  [32][33][34]

ForEachNeighborIndex(23, visitor):
  r=1: c=2→12, c=3→13, c=4→14
  r=2: c=2→22, c=3→23, c=4→24
  r=3: c=2→32, c=3→33, c=4→34
```

---

## Flow 6: Actor Creation & ID Generation

```
ActorRegistry::create_actor<PlayerActor>(connection):
  1. Dequeue free index from moodycamel::ConcurrentQueue
  2. Load version[index] (atomic relaxed)
  3. Construct ActorID{index, version}
  4. PlayerActor::create(id, connection)
     → alloc from ObjectPool<PlayerActor>
     → constructor receives ActorID → base_reset(id)
  5. actors_[index].store(actor, memory_order_release)
  6. Return ActorID

ActorRegistry::remove(id):
  1. actors_[index].store(nullptr, memory_order_release)
  2. actor->finalize()
     → PooledActor<T>::finalize()
       → ObjectPool<T>::release(ptr)
  3. versions_[index].fetch_add(1)  // ABA prevention
  4. free_indices_.enqueue(index)
```

---

## Flow 7: RPC Coroutine Call

```
PlayerActor (coroutine)            RoomManager (another worker)
      │                                │
      │  co_await RpcCall<ResT>(       │
      │    room_mgr_id, rpc_msg)        │
      │  ├─ RpcAwaiter                  │
      │  │  ├─ RpcManager::next_id()    │
      │  │  ├─ register pending + timer │
      │  │  └─ dispatch_msg(target,msg)─► handle_message()
      │  │                              │   └─ msg.Reply(res)
      │  │                              │   → RpcResponseMsg(rpc_id)
      │  │  ◄─── RpcResponseMsg ────────│
      │  │  ├─ RpcManager::consume()    │
      │  │  └─ handle.resume()          │
      │  ├─ await_resume() → result     │
      │  └─ continue handler            │
      │                                │
      │  On timeout (5s default):      │
      │  ├─ RpcManager::on_timeout()   │
      │  └─ await_resume() → exception │
```

---

## Flow 8: Camp Create/Join Flow

```
PlayerActor                       GateServer (Worker)        RoomManager
     │                                    │                      │
     │  C2S_CreateCampReq                  │                      │
     │  ───────────────────────────►       │                      │
     │                         Dispatcher  │                      │
     │                                    │  ───RPCAssignCampReq──►│
     │                                    │   (co_await RpcCall)    │
     │                                    │                      │on_assign_camp()
     │                                    │                      │ ├─ check camp_name
     │                                    │                      │ ├─ create_actor<SceneActor>
     │                                    │                      │ └─ Reply(AssignCampRes)
     │                                    │  ◄───RPCAssignCampRes──│
     │                                    │                      │
     │                                    │  ├─ SceneLeaveMsg (old scene)
     │                                    │  ├─ SceneEnterMsg (new scene)
     │  ◄──S2C_CreateCampRes─────────────│                      │
     │                                    │                      │
     │  [Every 3s: SceneActor sends        │                     │
     │   CampPlayerCountMsg to RoomManager] │                    │
     │                                    │                      │on_camp_player_count()
     │                                    │                      │ └─ update camp_metas_
     │                                    │                      │
     │  [C2S_QueryCampListReq → S2C_QueryCampListRes]            │
     │  Directly reads RoomManager::camp_metas() cache           │
```

---

## Flow 9: Dispatcher Dual Path

```
              Dispatcher (singleton)
                    │
        ┌───────────┴───────────┐
        │                       │
   Net Path                 RPC Path
  (from client)           (actor-to-actor)
        │                       │
  register_handler<      register_rpc<
    ProtoMsg>               RpcMsgType>
        │                       │
  ParseFromArray →       static_cast ←
  protobuf        →       zero-cost  ←
  handler(actor, msg)    handler(actor, msg)
```

---

## Flow 10: NpcActor AI Sleep/Wake

```
SceneActor::OnTick() — every 50ms
  │
  ├─ sync_mgr_.Tick(...) ← state sync (players only)
  │
  └─ For each NPC:
       │
       ├─ AOI::GetViewEntityIds(npc_grid) → view_entities
       ├─ Any PlayerActor in view_entities?
       │   ├─ Yes → npc->SetAiActive(true)
       │   └─ No  → npc->SetAiActive(false)
       └─ npc->OnTick()
            ├─ if (!is_ai_active_) return;  // 99% cpu saved
            └─ BT::Tree::tickWhileRunning()
```

---

## Flow 11: Connection Shutdown

```
Client disconnects → io_uring CQE (EOF/error)

Connection::read_packet() co_return nullptr
  → GateServer::handle_session() exits read loop
    → dispatch_to_actor(player, SessionClosedMsg(0))
      → PlayerActor::on_session_closed(reason)
        → SceneLeaveMsg → SceneActor removes from AOI
        → conn_.reset() → Socket destructor → close(fd)
    → Connection shared_ptr freed (no more coroutines holding it)
```
