# Aegis Engine: Room Manager (监督者与 RPC)

### 1. 模块定义 (What & Why)

- **一句话定位：** `RoomManager` 是全局单例 Actor，扮演 **Supervisor (监督者)** 的角色。它负责管理所有战斗场景 (`SceneActor`) 的生命周期（创建、销毁、故障监控）。
    
- **设计初衷 (Motivation)：**
    
    - **Supervisor Pattern (Erlang 哲学)：** "Let it crash"。具体的业务 Actor (`SceneActor`) 可能会因为逻辑 bug (如除以零) 崩溃。我们需要一个父节点 (`RoomManager`) 来接收死亡通知，清理资源，甚至重启它，防止单点故障导致整个进程退出。
        
    - **统一入口：** 客户端并不直接连接 `SceneActor`（因为还没创建），它们连接 `RoomManager` 来申请房间，获取 IP/Port/Token，然后才是进入场景。
        

### 2. 核心技术决策 (Key Decisions & Trade-offs)

#### A. 内存策略：SimpleActor vs PooledActor

- **决策：** 继承自 `SimpleActor<RoomManager>`。
    
- **理由：**
    
    - **单例特性：** 全服只有一个 `RoomManager`。
        
    - **无复用需求：** 它随服务器启动而生，随服务器关闭而死。
        
    - **Trade-off：** 为其分配一个 1000 容量的 `ObjectPool` 是极大的内存浪费。直接使用 `new/delete` 是最经济的选择。
        

#### B. 监管机制：父子绑定 (Parent-Child Linking)

- **代码特征：** `scene->set_parent_id(this->id())`。
    
- **机制：**
    
    - 在创建 `SceneActor` 后，立即确立父子关系。
        
    - 当 `SceneActor` 调用 `finalize` (正常销毁) 或发生未捕获异常 (异常退出) 时，调度器会检测其 `parent_id`。
        
    - 调度器会自动向 Parent 发送 `MSG_TYPE_ACTOR_DIED` 消息。
        
- **收益：** 实现了**自动化的生命周期闭环**。`RoomManager` 不需要轮询检查“房间还在不在”，而是被动等待通知。
    

#### C. 双向索引 (Bi-directional Indexing)

- **决策：** 维护两个 `unordered_map`：`RoomID -> ActorID` 和 `ActorID -> RoomID`。
    
- **理由：**
    
    - **O(1) 查找：** 处理 RPC 时用 RoomID 查 Actor。
        
    - **O(1) 清理：** 处理 `ACTOR_DIED` 时，消息里只有 `ActorID`（死者的 ID）。如果不存反向索引，就得遍历整个 Map 来删 RoomID，那是 O(N) 的灾难。
        

### 3. 关键实现细节 (Implementation Deep Dive)

#### A. 异步系统中的“同步” RPC

虽然 Actor 是全异步的，但 `create_room` 这种业务通常需要立即知道结果（成功/失败）。

- **实现：**
    
    - `RpcMessage` 携带了一个 `std::promise`。
        
    - `RoomManager` 处理完逻辑后，调用 `msg.Reply(res)` 填充 promise。
        
    - 发送端（通常是 GateServer 或测试代码）持有 `future` 并阻塞等待（或 `co_await`）。
        
- **注意：** 这种模式只适用于**控制流**（Control Plane），绝对不能用于**数据流**（Data Plane，如移动同步），因为 Promise/Future 有显著的性能开销。
    

#### B. 状态一致性 (State Consistency)

在 `on_terminate_room` 中：

```
// 1. 先删 Map (逻辑移除)
room_id_to_actor_.erase(it);

// 2. 再发消息让 Actor 自杀
scene_ptr->push(new ActorDestroyMsg());
```

- **细节：** 为什么要先删 Map？
    
    - 如果先发消息，Actor 自杀需要时间（可能要几毫秒排队）。
        
    - 在这几毫秒内，如果又有玩家请求加入该房间，查 Map 发现还在，请求发给 Scene，Scene 却正在析构，可能导致错误。
        
    - **先斩后奏：** 先在逻辑上宣判死亡，阻止新请求，然后再执行物理销毁。
        

### 4. 踩坑与难点 (Challenges & Solutions)

#### 难点 1：僵尸房间 (Zombie Rooms)

- **场景：** `SceneActor` 因为 C++ 异常（如 `vector` 越界）直接抛出，跳过了正常的 `handle_message` 流程。
    
- **解决：**
    
    - 依赖 `Actor::process_batch` 中的 `try-catch` 块。
        
    - 捕获异常后，系统强制调用 `finalize` 并发送 `ACTOR_DIED (reason=CRASH)` 给 `RoomManager`。
        
    - `RoomManager::on_scene_died` 收到非 0 的 reason，打印 Error Log 并清理 Map，确保不会内存泄漏或句柄残留。
        

#### 难点 2：ID 转换的安全性

- **问题：** `SceneActor` 的 ID 是 64 位的（含 Version），但 Map 里为了省内存可能只存了 32 位 Index（假设）。
    
- **Aegis 策略：**
    
    - `RoomManager` 存储的是 `scene_id.raw` (64位)。
        
    - 在 `get` 时，`Registry` 会校验 Version。
        
    - 如果 Map 里存的是旧 ID（比如上次崩溃没清理干净），Registry 会返回 `nullptr`，避免了向错误的（复用的）新 Actor 发消息。
        

### 5. 性能复杂度 (Complexity)

- **Create/Terminate:** **O(1)** (Map 操作 + Registry 操作)。
    
- **Supervision:** **O(1)** (Map 操作)。
    
- **空间复杂度：** `O(N)`，N 为活跃房间数。
    

### 6. 面试模拟 (Interview Q&A)

**Q: `SimpleActor` 和 `PooledActor` 的区别是什么？**

**A:** 主要区别在于内存分配策略。`PooledActor` 使用对象池（Slab Allocator），适合高频创建销毁的对象（如玩家、子弹），能避免内存碎片。`SimpleActor` 直接使用 `new/delete`，适合全服单例或极低频的对象（如 `RoomManager`）。`RoomManager` 选 `SimpleActor` 是因为给它开一个几万大小的池子纯属浪费。

**Q: 如果 `SceneActor` 崩溃了，`RoomManager` 怎么知道？**

**A:** 我们实现了类似 Erlang 的 Supervisor 模式。在创建 `SceneActor` 时，通过 `set_parent_id` 绑定了父子关系。底层的 Actor 调度器捕获到子节点的异常或销毁事件后，会自动构建一个 `MSG_TYPE_ACTOR_DIED` 消息发给父节点。`RoomManager` 在 `handle_message` 里处理这个消息，根据 ID 清理路由表，并记录崩溃日志。

**Q: 为什么创建房间要用 RPC？直接发个消息不行吗？**

**A:** 这里用 RPC 是为了业务层的便利性。创建房间通常是 HTTP 接口或 GateServer 发起的，调用者需要拿到 `RoomID` 和 `IP/Port` 才能返回给客户端。如果用纯异步消息，调用者需要维护一个复杂的上下文 Map 来匹配 Request 和 Response。使用封装了 `std::promise` 的 RPC Message，可以让调用者写出同步风格的代码（或 `co_await`），逻辑更线性。