# Aegis Engine: Scene Actor (场景大管家)

### 1. 模块定义 (What & Why)

- **一句话定位：** `SceneActor` 是一个具体的业务 Actor，负责管理一张地图（或地图的一个分片）内的**实体生命周期**、**空间索引 (AOI)** 和 **视野广播**。
    
- **设计初衷 (Motivation)：**
    
    - **并发瓶颈碎裂：** 如果全服在一张大地图里，加锁会极其复杂。通过将地图划分为多个 `SceneActor`（分线或分区域），利用 Actor 模型的排队机制，将空间计算隔离在单线程内。
        
    - **消除锁竞争：** 场景内的移动、进入、离开涉及大量容器操作 (`std::vector`, `std::unordered_map`)。因为 Actor 保证串行执行，我们完全不需要 `std::mutex`。
        

### 2. 核心技术决策 (Key Decisions & Trade-offs)

#### A. 状态管理：Raw Pointer 缓存

- **决策：** 内部成员 `std::unordered_map<uint64_t, PlayerActor *> actors_` 直接持有玩家指针，而不是 ID。
    
- **理由：**
    
    - **极致性能：** 广播时需要遍历邻居列表并发送消息。如果存 ID，每次都要去 `Registry::get()` 查一次，开销太大。
        
    - **安全性保障：** 虽然持有裸指针看似危险，但在 Actor 模型下，只要保证 **"玩家销毁前必须先发消息通知场景离开"** 这一协议，指针就是安全的。
        

#### B. 内存复用：Member Cache Vectors

- **决策：** `cachedEnterIds_` 和 `cachedLeaveIds_` 是成员变量，而不是 `OnHandleMove` 里的局部变量。
    
- **理由 (面试加分项)：**
    
    - **Zero Allocation Loop：** 游戏中的移动是超高频事件。如果每次移动都 `std::vector<uint64_t> results`，会频繁触发 `malloc/free`。
        
    - **做法：** 成员变量长期持有内存 (`reserve`)，每次使用前 `clear()`（不释放内存），实现了热路径上的**零内存分配**。
        

#### C. AOI 策略：同步计算 vs 异步服务

- **决策：** AOI 逻辑 (`aoi_grid`) 直接嵌入在 `SceneActor` 内部同步执行，而不是拆分为独立的 AOI 服务。
    
- **取舍：**
    
    - **优势：** 数据局部性好，拿完 AOI 结果立刻就能从 `actors_` 表里取到对象发消息，延迟极低。
        
    - **劣势：** 如果单地图人数过多（如 > 2000人），AOI 计算会阻塞消息处理。解决方案是做地图分片（Grid Sharding）。
        

### 3. 关键实现细节 (Implementation Deep Dive)

#### A. 移动逻辑的三段式处理 (The Move Protocol)

`OnHandleMove` 是整个 MMO 逻辑中最复杂的一环，代码处理得非常清晰：

1. **AOI 更新与 Diff 计算：**
    
    - 调用 `aoi_.Move(...)`。
        
    - **关键：** 同时传出 `cachedEnterIds_` (新看见的人) 和 `cachedLeaveIds_` (看不见的人)。这是一个原子操作，避免了分别调用 Move 和 GetView 导致的状态不一致。
        
2. **视野变更处理 (Enter/Leave View)：**
    
    - 对 `cachedEnterIds_`：互相发 `SC_ENTER_VIEW`（带上坐标和外观）。
        
    - 对 `cachedLeaveIds_`：互相发 `SC_LEAVE_VIEW`（通常只发 ID）。
        
3. **位置同步 (Move Broadcast)：**
    
    - 对 `GetViewEntityIds` 里的**剩余**邻居：广播 `SC_MOVE_NTF`。
        
    - **优化：** 这里包含了一点冗余（刚才 Enter 的人也会收到 Move），但简化了逻辑。客户端通常具备幂等性处理能力。
        

#### B. 消息广播 (Broadcasting)

```
void SendBuffer(uint64_t targetId, ...) {
    if (auto actor = GetPlayer(targetId)) {
        actor->send_buffer(...); 
    }
}
```

- 这体现了 Actor 间的通信模式：**不直接调用对方逻辑，只向对方邮箱投递数据**。
    
- `send_buffer` 是线程安全的（因为它是 Connection 的 Outbox 入队操作，带锁/CAS）。
    

### 4. 踩坑与难点 (Challenges & Solutions)

#### 难点 1：广播风暴 (Broadcast Storm)

- **场景：** 100 个人聚在一起，一个人动一下，要发 99 个包。100 个人同时动，一秒钟就是 100 * 99 * 10 (Hz) = 10万个包。
    
- **Aegis 应对：**
    
    - **Protobuf 序列化复用：** `ntf.SerializeAsString()` 在循环外只做一次。循环内只做内存拷贝 (`send_buffer`)。这比在循环内对每个邻居都 `Serialize` 一次快 10 倍。
        
    - **批处理 (Batching)：** 之前在 `Network` 层看到的 `OutboxBatcher` 在这里发挥了巨大作用，这些小包会被自动合并成大块发送给 OS。
        

#### 难点 2：幽灵玩家 (Zombie Players)

- **场景：** 玩家直接断网（不发 Leave 消息），场景不知道，AOI 里永远留着这个“幽灵”。
    
- **解决：** 需要在 `PlayerActor` 的析构或断线逻辑中，强制向 `SceneActor` 发送 `SCENE_LEAVE` 消息。这是分布式系统一致性的典型要求。
    

### 5. 性能复杂度 (Complexity)

- **Move 操作：** `O(K + M)`。
    
    - K = AOI 算法复杂度 (九宫格通常是 O(1) 或 O(N_neighbors))。
        
    - M = 视野内邻居数量 (用于广播)。
        
- **Enter/Leave：** `O(M)`。
    

### 6. 面试模拟 (Interview Q&A)

**Q: 为什么 `SceneActor` 里没有锁？**

**A:** 这正是 Actor 模型的精髓。`SceneActor` 本质上是一个串行执行的队列消费者。同一时刻，调度器只允许一个线程执行这个 Actor 的 `handle_message`。因此，其内部的所有成员变量（AOI、玩家列表、缓存Vector）都是被当前线程独占访问的，完全不存在竞争，自然不需要锁。

**Q: 在 `OnHandleMove` 里，你是先处理 Move 还是先处理 Enter/Leave？**

**A:** 这是一个经典的时序问题。在代码中，`aoi_.Move` 会同时返回 diff（Enter/Leave 列表）。逻辑上，我们通常**先处理视野变化**（告诉客户端“有人来了”或“有人走了”），**最后广播移动**。这样客户端能先创建实体，再平滑插值移动。如果反过来，客户端可能收到一个“未知实体的移动包”，导致逻辑错误。

**Q: `actors_` map 存指针安全吗？如果玩家掉线了怎么办？**

**A:** 在我们的架构中，这是安全的。因为 `PlayerActor` 的生命周期被严格管理。当玩家掉线时，`PlayerActor` 会向 `SceneActor` 发送 `MSG_TYPE_SCENE_LEAVE` 消息。由于 Actor 消息是顺序处理的，`SceneActor` 处理完 Leave 消息并移除指针后，`PlayerActor` 才会真正销毁。我们通过消息流转保证了生命周期的同步。