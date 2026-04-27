# 营地系统 — RoomManager / GameApp / RPC 协程

> 对应目录: `include/aegis/game/room_manager.h` · `include/aegis/game/game_app.h` ·
> `include/aegis/game/rpc_awaiter.h` · `src/game/room_manager.cpp` · `src/game/game_app.cpp`

---

## 1. 架构概览

```
[客户端]                           [GateServer]                            [服务端]
  │                                    │                                       │
  │  C2S_CreateCampReq / JoinCampReq   │                                       │
  │ ──────────────────────────────────►│                                       │
  │                                    │  PlayerActor                          │
  │                                    │    │                                  │
  │                                    │    ▼  RPC (co_await RpcCall)          │
  │                                    │  ┌──────────────────────────────┐    │
  │                                    │  │  RoomManager                  │    │
  │                                    │  │  ├─ camp_metas_ 缓存          │    │
  │                                    │  │  ├─ on_assign_camp()          │    │
  │                                    │  │  └─ 创建/查询 SceneActor      │    │
  │                                    │  └──────────────────────────────┘    │
  │                                    │         │                            │
  │                                    │         ▼ 创建                       │
  │                                    │  ┌──────────────┐                   │
  │                                    │  │  SceneActor   │  (新营地)         │
  │                                    │  │  AOI + NPC    │                   │
  │                                    │  └──────────────┘                   │
  │  S2C_CreateCampRes / JoinCampRes   │         │                            │
  │ ◄──────────────────────────────────│         │  人数上报 (每 3s)          │
  │                                    │         ▼                            │
  │                                    │  RoomManager::camp_metas_ 更新       │
```

---

## 2. GameApp (`game_app.h`)

集中化的游戏业务启动器，替代了之前散落在 GateServer 中的业务初始化逻辑。

```cpp
class GameApp {
    static GameApp& instance();
    void init(float map_width = 500.0f,
              float map_height = 500.0f,
              float cell_size = 10.0f,
              int worker_id = 3);

    ActorID default_scene_id() const;   // 默认主城 Scene ActorID
    ActorID room_manager_id() const;    // RoomManager ActorID
};
```

**init() 执行流程**:
1. 创建 `RoomManager` (SimpleActor)
2. 创建主城 `SceneActor` (默认大小 500x500, cell=10)
3. 在主城 NPC（玩家登录后进入主城）

**main.cpp 启动顺序**:
```cpp
int main() {
    GateServer server;
    server.init("logs/gate_server.log", 4);       // 1. 网关初始化
    GameApp::instance().init();                     // 2. 游戏业务启动
    server.run(8888);                               // 3. 开始接受连接
}
```

---

## 3. RoomManager (`room_manager.h`)

```cpp
class RoomManager : public SimpleActor<RoomManager> {
    // 双向映射
    unordered_map<uint32_t, uint32_t> room_id_to_actor_; // 房间ID → ActorID
    unordered_map<uint32_t, uint32_t> actor_to_room_id_; // ActorID → 房间ID

    // 营地元数据缓存
    unordered_map<uint64_t, CampMeta> camp_metas_;

    unordered_set<uint64_t> destroying_scenes_; // 销毁中的场景

    void on_assign_camp(const RPCAssignCampMsg &msg);
    void on_camp_player_count(const CampPlayerCountMsg &msg);
    void on_scene_died(uint64_t deceased_id, int reason);
};
```

### CampMeta

```cpp
struct CampMeta {
    uint64_t scene_actor_id = 0;
    std::string camp_name;
    int32_t current_players = 0;
    int32_t max_players = 20;  // 硬限制
};
```

### on_assign_camp — 营地的核心分配逻辑

**创建模式 (`is_create=true`)**:
1. 检查玩家传入的 camp_name
2. 创建新的 SceneActor（自动注册到 ActorRegistry）
3. 缓存 CampMeta → `camp_metas_`
4. 回复 AssignCampRes（含 scene_actor_id）

**加入模式 (`is_create=false`)**:
1. 通过 `target_scene_id` 查找目标 SceneActor
2. 检查缓存中该 SceneActor 的当前人数
3. 若未满，返回该 Scene 的 ActorID；若满，回复错误

### on_camp_player_count — 营地人数上报

由 SceneActor 每 3 秒发送 `CampPlayerCountMsg` 到 RoomManager：
```cpp
void on_camp_player_count(const CampPlayerCountMsg &msg) {
    auto it = camp_metas_.find(msg.scene_actor_id);
    if (it != camp_metas_.end())
        it->second.current_players = msg.player_count;
}
```

---

## 4. RPC 协程 (`rpc_awaiter.h`)

### 4.1 设计动机

**旧方式（阻塞 Worker）**:
```cpp
auto future = rpc_msg->promise.get_future();
dispatch_msg(target, rpc_msg);
AssignCampRes res = future.get();  // ⚠️ 阻塞 Worker 线程！
```

**新方式（协程异步）**:
```cpp
AssignCampRes res = co_await RpcCall<AssignCampRes>(target_id, rpc_msg);
// 不阻塞 Worker！协程挂起，其他 Actor 可以在这期间继续工作
```

### 4.2 RpcManager — 每个 Worker 的 RPC 挂起管理器

```cpp
class RpcManager {
    thread_local static RpcManager instance();

    void register_pending(RpcId id, coroutine_handle<> handle,
                          void** slot, uint32_t timer_id);
    coroutine_handle<> consume(RpcId id, void* result_ptr);
    void on_timeout(RpcId id);

    RpcId next_id();  // 格式: [16-bit WorkerID][48-bit Counter]
};
```

### 4.3 RpcAwaiter — 可等待对象

```cpp
template<typename ResT>
class RpcAwaiter {
    ActorID target_id_;
    ActorMessage *msg_;
    RpcIdSetter rpc_id_setter_;
    uint32_t timeout_ms_;  // 默认 5000ms

    bool await_ready()    { return false; }
    bool await_suspend(handle) {
        rpc_id_ = RpcManager::next_id();
        rpc_id_setter_(rpc_id_);          // 写回 RpcMessage
        register_pending + 超时定时器;
        dispatch_msg(target, msg_);       // 发送 RPC 请求
        return true;                      // 挂起当前协程
    }
    ResT await_resume() {
        if (timed_out_ || result_ == nullptr)
            throw runtime_error("RPC timed out");
        return std::move(*static_cast<ResT*>(result_));
    }
};
```

### 4.4 RpcCall 便捷函数

```cpp
template<typename ResT, typename MsgT>
RpcAwaiter<ResT> RpcCall(ActorID target, MsgT *msg,
                          uint32_t timeout_ms = 5000);
```

### 4.5 RPC 完整调用流

```
                                           ┌──────────────────────┐
PlayerActor 协程                          │    RoomManager       │
  │                                       │   (目标 Actor)       │
  │  co_await RpcCall(target, msg)        │                      │
  │  ├─ RpcAwaiter::await_suspend()       │                      │
  │  │  ├─ 生成 rpc_id                    │                      │
  │  │  ├─ 注册挂起到 RpcManager          │                      │
  │  │  ├─ 注册超时定时器                  │                      │
  │  │  └─ dispatch_msg(target, msg) ────►│ handle_message()     │
  │  │                                    │   └─ msg.Reply(res)  │
  │  │                                    │   → RpcResponseMsg   │
  │  │                                    │     (投递回发起者)    │
  │  │  ◄─── RpcResponseMsg ──────────────│                      │
  │  │  ├─ RpcManager::consume(rpc_id)    │                      │
  │  │  └─ handle.resume()                │                      │
  │  ├─ await_resume() → 返回 ResT        │                      │
  │  └─ 继续执行业务逻辑                    │                      │
  │                                       │                      │
  │  超时触发 (5s):                        │                      │
  │  ├─ RpcManager::on_timeout(rpc_id)    │                      │
  │  └─ await_resume() → 抛异常            │                      │
```

---

## 5. 客户端协议消息

**营地创建**:
```
C2S: C2S_CreateCampReq  { camp_name, map_id }
S2C: S2C_CreateCampRes  { ret_code, msg, scene_actor_id, camp_name }
```

**营地加入**:
```
C2S: C2S_JoinCampReq    { scene_actor_id }
S2C: S2C_JoinCampRes    { ret_code, msg, scene_actor_id }
```

**营地列表查询**:
```
C2S: C2S_QueryCampListReq { page_index }
S2C: S2C_QueryCampListRes { camps[], total_count }
```

**营地快照补帧**:
```
S2C: S2C_CampSnapshotPush { scene_actor_id, players[] }
```
- 用于重连或初次进入营地时全量下发当前场景玩家状态
