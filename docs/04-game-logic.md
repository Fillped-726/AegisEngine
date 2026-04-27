# 游戏逻辑 — PlayerActor / SceneActor / NpcActor / AOI / SyncManager

> 对应目录: `include/aegis/game/` · `src/game/`
> 此目录从 `include/aegis/core/` 迁移而来，将游戏逻辑与框架核心分离。

---

## 1. PlayerActor (`playerActor.h`)

```cpp
class PlayerActor : public PooledActor<PlayerActor> {
    // ── 网络连接 ──
    std::shared_ptr<net::Connection> conn_;  // TCP 连接
    int fd_;

    // ── 身份 ──
    uint64_t playerId_;        // 业务 UID（Login 时设置）

    // ── 坐标 (原子，支持跨线程读) ──
    std::atomic<float> x_{0.0f};
    std::atomic<float> y_{0.0f};
    float z_{0.0f};

    // ── 属性 ──
    int32_t hp_;
    PlayerState state_{IDLE};
    uint32_t aoi_grid_index = -1;

    // ── 脏标记 ──
    uint8_t dirty_mask_ = DIRTY_NONE;
    enum DirtyFlag : uint8_t {
        DIRTY_NONE  = 0,
        DIRTY_POS   = 1 << 0,    // 坐标变化
        DIRTY_HP    = 1 << 1,    // 血量变化
        DIRTY_STATE = 1 << 2,    // 状态变化
    };
};
```

### 核心方法

| 方法 | 说明 |
|------|------|
| `SetPos(x, y, z, dirty_flag)` | 原子更新坐标 + 脏标记 |
| `GetX() / GetY()` | 原子读取坐标（`memory_order_relaxed`） |
| `TakeDamage(amount)` | 扣血，归零触发 DEAD 状态 |
| `MarkDirty / ClearDirty / HasAnyDirty` | 脏标记操作 |
| `WriteToProto(proto, snapshotX, snapshotY)` | 将自身状态序列化到 protobuf |
| `send_packet<T>(msg_id, seq_id, msg)` | 发送 protobuf 包 |
| `send_buffer(msg_id, seq_id, serialized_data)` | 发送预序列化 buffer（广播优化） |

### handle_message 分发逻辑

```cpp
switch (msg->type_id) {
    case MSG_TYPE_NETWORK:          // 客户端网络消息 → Dispatcher::dispatch()
    case MSG_TYPE_SESSION_CLOSED:   // 断开连接 → on_session_closed()
    case MSG_TYPE_FORWARD_PACKET:   // 广播转发 → send_buffer()
    case MSG_TYPE_RPC_RESPONSE:     // RPC 响应 → RpcManager::consume()
    default: break;
}
```

### 断开处理 (on_session_closed)

1. 通知父场景移除自己（`SceneLeaveMsg`）
2. 释放 `conn_`（shared_ptr 递减 → 关闭连接）

---

## 2. SceneActor (`scene_actor.h`)

```cpp
class SceneActor : public PooledActor<SceneActor, 128, 32> {
    AOIGrid aoi_;
    std::unordered_map<uint64_t, PlayerActor*> actors_;  // 玩家 (raw_id → 指针)
    std::unordered_map<uint64_t, NpcActor*> npcs_;        // NPC
    SyncManager sync_mgr_;
    bool is_ticking_ = false;
    uint32_t report_counter_ = 0;  // 人数上报计数器
};
```

### handle_message 分发

```cpp
switch (msg->type_id) {
    case MSG_TYPE_SCENE_ENTER:      OnHandleEnter(msg);
    case MSG_TYPE_SCENE_LEAVE:      OnHandleLeave(msg);
    case MSG_TYPE_SCENE_MOVE:       OnHandleMove(msg);
    case MSG_TYPE_SCENE_SKILL_CAST: OnHandleSkillCast(msg);
}
```

### OnHandleEnter — 进入场景逻辑

1. 如果场景尚未 Tick，启动定时器（50ms）
2. 将玩家注册到 `actors_` 映射、AOI 网格
3. 通知周围邻居「新玩家来了」（via `ForwardPacketMsg + SC_ENTER_VIEW`）
4. 通知新玩家「周围有谁」（邻居的 EnterView 快照）

### OnHandleMove — 移动处理

**关键设计**: OnHandleMove 只更新坐标 + 标记脏，不立即发包！

```cpp
void OnHandleMove(SceneMoveMsg *msg) {
    mover->SetPos(msg->newX, msg->newY, 0.0f, msg->direction);
    sync_mgr_.AddDirtyPlayer(mover);  // 延迟到 Tick 时广播
}
```

### OnTick — 定时驱动 (50ms)

```
1. sync_mgr_.Tick(aoi, enterLeaveCb, sendCb)  ← 状态同步
2. NPC 动态休眠检查 + NPC::OnTick()            ← AI 驱动
3. 重新注册定时器                               ← 继续 Tick
4. 每 60 Tick (~3s) 上报人数给 RoomManager      ← 营地人数统计
```

### ProcessAoiEnterLeave — AOI 跨格视野变更

当玩家移动跨越 AOI 网格边界时:
1. 通知新格子的邻居「有新人进入」（EnterView）
2. 通知旧格子的邻居「有人离开」（LeaveView）
3. 通知移动者「新的邻居们」

---

## 3. NpcActor (`npc_actor.h`)

```cpp
class NpcActor : public PooledActor<NpcActor, 256, 64> {
    BT::Tree tree_;          // BehaviorTree.CPP v4 行为树
    bool is_ai_active_ = false;  // AOI 驱动的休眠标志
    SceneActor* scene_;      // 所属场景（用于 BT 节点回调查询）
};
```

**AI 休眠机制**:
- `is_ai_active_` 由 SceneActor 在 OnTick 中设置
- `npc->OnTick()` 内部 `if (!is_ai_active_) return;` — 99% 的 CPU 节省
- 大世界 90%+ 的 NPC 周围可能无玩家，不执行行为树

---

## 4. AOI Grid (`aoi_grid.h`)

```cpp
class AOIGrid {
    // 坐标 → 网格索引: O(1)
    uint32_t Add(uint64_t entity_id, float x, float y);
    bool RemoveByGridIndex(uint64_t entity_id, uint32_t grid_index);
    uint32_t Move(uint64_t entity_id, uint32_t old_grid, float newX, float newY,
                  vector<uint64_t>& outEnter, vector<uint64_t>& outLeave);

    // 9 宫格查询
    void GetViewEntityIds(uint32_t grid_index, vector<uint64_t>& out);
    void ForEachNeighborIndex(uint32_t center, auto visitor);

    SpinLock lock_;  // 线程安全
};
```

**网格查询**:
```
World: 1000x1000, cellSize=100 → 10x10 = 100 cells

Player at (350, 250):
  col = 350/100 = 3, row = 250/100 = 2
  centerGridIndex = 2*10 + 3 = 23

9-grid neighbors (col 2-4, row 1-3):
  [12][13][14]
  [22][23][24]  ← center
  [32][33][34]
```

---

## 5. SyncManager (`sync_manager.h`)

脏标记驱动的状态同步引擎，模板化设计解耦算法与发送逻辑。

```cpp
class SyncManager {
    vector<PlayerActor*> dirty_players_;
    unordered_set<uint64_t> dirty_set_;

    template<typename EnterLeaveFunc, typename SendBatchFunc>
    void Tick(AOIGrid& aoi, EnterLeaveFunc&& onEnterLeave, SendBatchFunc&& onSendBatch);
};
```

### Tick 四阶段

```
Phase 1: AOI Move + Enter/Leave 计算
  for 每个脏玩家 (DIRTY_POS):
    AOI::Move() → 如果跨格 → onEnterLeave(mover, enterIds, leaveIds)

Phase 2: 聚合接收者批次
  for 每个脏玩家:
    查 9 宫格邻居
    for 每个邻居 != 自己:
      receiver_batches[neighbor].add_move(mover_info)

Phase 3: 批量发送
  for 每个 (receiver, batch):
    shared_buf = make_shared<string>(batch.SerializeAsString())
    onSendBatch(receiverActorId, shared_buf)

Phase 4: 清理脏标记
  for 每个脏玩家: ClearDirty()
  dirty_players_.clear()
  dirty_set_.clear()
```

**设计亮点**:
- 模板 Tick 只负责「算法」，不负责「发送」— 通过回调解耦
- 每个接收者**一次序列化**，shared_ptr 避免拷贝
- 预分配 `dirty_players_.reserve(1024)` 避免 Tick 中动态扩容
