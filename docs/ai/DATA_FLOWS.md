# AegisEngine Data Flow Diagrams

> Protocol flow, cross-thread messaging, and hot paths for AI code understanding.

## Flow 1: TCP Connection → Packet → Handler

```
Client                          Acceptor                  Worker
  │                                │                        │
  │  ──TCP SYN─────────────────►   │                        │
  │                                │  accept() coroutine    │
  │                                │  → Socket(accepted_fd) │
  │                                │  → Connection(socket)  │
  │                                │  → create_actor<PlayerActor>(connection)
  │                                │  → ActorRegistry::register(playerID)
  │                                │                        │
  │  ──[4B len][4B msg_id][body]  │                        │
  │  ─────────────────────────►   │                        │
  │                                │  io_uring CQE          │
  │                                │  → Connection::read_packet() resumes
  │                                │  → Packet assembled    │
  │                                │  → actor->enqueue_message(NetworkMessage)
  │                                │                        │
  │                                │[Worker message drain]  │
  │                                │  → PlayerActor::handle_message()
  │                                │    → Dispatcher::dispatch(actor, packet)
  │                                │      → handler(actor, parsed_proto)
  │                                │        → co_await business_logic
  │                                │                        │
  │  ◄──[4B len][4B msg_id][body]  │                        │
  │        (response via conn_)    │                        │
```

## Flow 2: State Sync (SyncManager Tick)

```
Tick (50ms timer on SceneActor's worker)

  ┌────────────────────────────────────────────┐
  │ SyncManager::Tick(aoi, enterLeaveCb, sendCb)│
  └────────────────────────────────────────────┘
         │
         ▼
  Phase 1: For each dirty player with DIRTY_POS
  ─────────────────────────────────────────────
  AOI::Move(id, oldGrid, newX, newY, enterIds, leaveIds)
      ↓
  grid changed? → enterLeaveCb(mover, enterIds, leaveIds)
    → SceneActor::ProcessAoiEnterLeave()
      → PacketBuilder::BuildEnterView() / BuildLeaveView()
      → send_buffer() to entering/leaving players
      → (send_buffer internally prepends header + msg_id)
         │
         ▼
  Phase 2: For each dirty player
  ─────────────────────────────────────────────
  AOI::GetViewEntityIds(mover.grid, neighbors)
    For each neighbor != self:
      → receiver_batches[neighbor].add_move(mover.info)
         │
         ▼
  Phase 3: For each (receiverActorID, batch)
  ─────────────────────────────────────────────
  shared_buf = make_shared<string>(batch.SerializeAsString())
  → onSendBatch(receiverActorId, shared_buf)
    → SceneActor::SendSharedBuffer()
      → ForwardPacketMsg(msg_id, shared_buf)
      → enqueue_message to receiver's worker (may be different worker)
         │
         ▼
  Phase 4: ClearDirty for all processed players
         │
         ▼
  PlayerActor receives ForwardPacketMsg
    → handle_message(MSG_TYPE_FORWARD_PACKET)
      → send_buffer(msg_id, *shared_buf)
        → Connection::send(PooledPacket)
          → Outbox + flush → writev via io_uring
```

## Flow 3: Cross-Worker Message Delivery

```
Worker A (SceneActor)          Worker B (PlayerActor)
      │                              │
      │  enqueue_message(            │
      │    ForwardPacketMsg)          │
      │  ──────────────────────────►  │
      │                              │ Worker B polls its msg queue
      │                              │ → PlayerActor::handle_message()
      │                              │ → send_buffer() → socket write
      │                              │
```

Key: `Actor::enqueue_message()` is thread-safe, using a lock-free intrusive list push. The receiver's worker drains its queue during its run loop.

## Flow 4: Coroutine Lifecycle

```
Caller:
  auto task = some_coroutine();
  // Starts eagerly in caller's context (initial_suspend = never)
  
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
```

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
