# Aegis Engine: Player Actor (玩家实体)

### 1. 模块定义 (What & Why)

- **一句话定位：** `PlayerActor` 是玩家在服务器内存中的**代理对象 (Avatar)**。它一一对应于一个客户端连接，负责维持会话状态、转发网络包以及执行具体的业务逻辑（如登录、背包操作）。
    
- **设计初衷 (Motivation)：**
    
    - **逻辑与网络解耦：** `Connection` 只管字节收发，不关心业务；`PlayerActor` 负责将字节流解释为业务行为。
        
    - **状态容器：** 它是玩家数据的“主权领土”。所有关于玩家的属性（HP、坐标、背包）都由这个 Actor 独占管理，从而避免了多线程修改玩家数据的锁竞争。
        

### 2. 核心技术决策 (Key Decisions & Trade-offs)

#### A. 坐标的原子性 (Atomic Coordinates)

- **决策：** 使用 `std::atomic<float> x_, y_` 存储坐标，而不是普通的 `float`。
    
- **理由 (面试高频)：**
    
    - **跨线程读写：** `PlayerActor` 自己（Worker A）会写坐标；但 `SceneActor`（Worker B）在广播 AOI 时会**并发读取**这个坐标。
        
    - **防止撕裂 (Tearing)：** 虽然在 x64 架构上对齐的 float 读写通常是原子的，但在 C++ 标准层面并不保证。如果发生撕裂（读取了 X 的高 16 位和旧 X 的低 16 位），会导致坐标瞬移到宇宙尽头。使用 `std::atomic` 提供了标准层面的内存安全保证。
        
    - **Relaxed 内存序：** `store/load` 使用 `memory_order_relaxed`，因为坐标本身只是一个数值，不需要与其他变量建立 Happens-Before 的同步关系，性能最高。
        

#### B. 协程的“发射后不管” (Fire-and-Forget)

- **决策：** `handle_message` 是同步函数，但业务逻辑 (`Dispatcher`) 是异步协程。
    
- **实现：** 使用 `launch_task` 辅助函数。
    
- **机制：**
    
    - 当 `handle_message` 收到网络包时，它调用 `launch_task`。
        
    - `launch_task` 立即启动一个分离的协程 (`DetachedTask`)。
        
    - **关键点：** 这意味着 `handle_message` 会立即返回，Actor 可以继续处理下一条消息，而那个业务协程（比如查数据库）挂起后会在未来某个时刻恢复执行。这实现了**非阻塞的业务流**。
        

#### C. 连接的共享所有权 (Shared Ownership)

- **决策：** 持有 `std::shared_ptr<net::Connection>`。
    
- **理由：**
    
    - 防止悬垂指针。网络底层（IO 线程）和逻辑层（Actor 线程）可能同时持有连接。如果 IO 线程因为断开连接销毁了对象，而 Actor 还在尝试发包，会导致崩溃。`shared_ptr` 保证了只要 Actor 还在用，连接对象就不会被回收。
        

### 3. 关键实现细节 (Implementation Deep Dive)

#### A. 快照写入 (Snapshot Protocol)

在 `WriteToProto` 中有一个细节：

```
void WriteToProto(..., float snapshotX = -999.0f, ...) {
    if (snapshotX > -900.0f) {
        // 使用场景传入的快照
        pos->set_x(snapshotX);
    } else {
        // 读取当前原子值
        pos->set_x(GetX());
    }
}
```

- **为什么这样做？**
    
    - 当 `SceneActor` 广播 AOI 时，它希望所有收到包的人看到的坐标是**那一瞬间**的状态。如果直接读 `GetX()`，可能 Scene 计算 AOI 时 X=100，但在序列化发包的那一微秒，Player 更新成了 X=101，导致客户端看到的画面出现抖动或不同步。
        
    - 这里的“快照”参数保证了**计算与表现的一致性**。
        

#### B. 广播优化

```
void send_buffer(uint32_t msg_id, const std::string &serialized_data)
```

- 这是一个为了性能妥协接口。通常我们只发 Proto 对象，但这允许 `SceneActor` 序列化一次 (`serialized_data`)，然后调用 `send_buffer` 把同一段二进制数据塞给 100 个 `PlayerActor` 的 `Connection`。这是解决广播风暴的关键路径。
    

### 4. 踩坑与难点 (Challenges & Solutions)

#### 难点 1：协程生命周期悬挂

- **风险：** `launch_task` 启动了一个协程，此时 `PlayerActor` 销毁了（玩家下线），但协程还在等待数据库返回。当协程恢复并试图访问 `this` 指针时，Crash！
    
- **解决方案 (Aegis 采用的策略)：**
    
    - 通常需要 `WeakPtr` 保护，或者在 `PlayerActor` 析构时取消所有关联的 token。
        
    - 在目前的精简实现中，依赖 `Connection` 的生命周期和逻辑层的严谨性（例如数据库回调检查 Actor 是否存活）。在生产环境中，这里通常会引入 `CancelToken`。
        

### 5. 性能复杂度 (Complexity)

- **Send Packet:** 实际上是无锁队列入队操作，**O(1)**。
    
- **Handle Message:** 仅仅是分发，**O(1)**。具体的业务复杂度取决于业务逻辑。
    

### 6. 面试模拟 (Interview Q&A)

**Q: 为什么 PlayerActor 要用对象池？**

**A:** 玩家上线下线是非常频繁的行为（尤其是手游的弱网环境导致频繁重连）。`PlayerActor` 大小可能有几百字节，频繁 `new/delete` 会造成堆碎片。使用 `ObjectPool` 配合 `reset()` 方法，可以将内存分配开销降为零，且提升缓存局部性。

**Q: `SceneActor` 访问 `PlayerActor` 的 `GetX()` 需要加锁吗？**

**A:** 不需要。虽然是跨线程访问，但我们使用了 `std::atomic<float>` 并配合 `memory_order_relaxed`。这保证了读取操作不会读到写了一半的脏数据（Tearing），同时避免了互斥锁 (`std::mutex`) 带来的巨大开销。对于游戏里的坐标同步，极短时间的“非强一致性”（读到了上一帧的坐标）是可以接受的，但“撕裂”和“锁阻塞”是不可接受的。

**Q: 如果 `handle_message` 里启动了协程，协程还没跑完，Actor 能处理下一个消息吗？**

**A:** 能！这正是 Actor + Coroutine 的强大之处。`handle_message` 只是同步地“启动”了协程（在这个框架下即 `launch_task`），然后立即返回。Actor 就会从邮箱取出下一条消息继续处理。那个被挂起的协程，之后会由调度器在同一个线程（或被窃取到其他线程）恢复执行。这实现了**单 Actor 内的并发处理**。