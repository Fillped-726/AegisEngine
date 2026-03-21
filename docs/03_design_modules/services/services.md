# Aegis Engine: Gate Server & Handler Logic (网关服务完整架构)

### 1. 模块定义 (What & Why)

- **一句话定位：** `GateServer` 是 Aegis 引擎的 **"启动器 (Bootstrap)"** 和 **"连接持有者"**，而 `HandlerLoader` 是业务逻辑的 **"装配车间"**。
    
- **职责边界：**
    
    - **GateServer 负责：** 进程生命周期管理、全局 Actor (RoomManager/Scene) 的初始化、TCP 连接的 Accept 与断开检测、定时器驱动。
        
    - **HandlerLoader 负责：** 具体的 Protocol Buffer 消息与 Actor 逻辑的映射 (Binding)。
        
- **依赖关系：**
    
    - **底层支撑：** `Core::Env` (io_uring), `Core::Scheduler` (Work Stealing).
        
    - **业务支撑：** `Core::RoomManager` (监管者), `Core::PlayerActor` (玩家实体).
        

### 2. 核心架构与类图 (Architecture)

#### GateServer 的双重身份

1. **System Bootstrapper:** 负责按顺序初始化 Log -> World -> Logic -> Scheduler -> IO。
    
2. **Connection Manager:** 维护 `accept_loop` 和成千上万个 `handle_session` 协程。
    

#### 核心协程模型

```
graph TD
    A[main] -->|Run| B(GateServer::run)
    B -->|Spawns| C{accept_loop}
    B -->|Spawns| D{timer_loop}
    C -->|On Connect| E[handle_session Coroutine 1]
    C -->|On Connect| F[handle_session Coroutine 2]
    E -->|Read| G(Connection::read_packet)
    G -->|Msg| H[PlayerActor::mailbox]
    H -->|Dispatch| I[Scheduler Worker]
```

### 3. 核心交互流程 (Key Workflows)

#### A. 服务器启动 (The Bootstrap Sequence)

这是面试中常问的 **"服务器启动都做了什么？"**：

1. **World Init:** 创建 `RoomManager` (上帝 Actor) 和默认 `SceneActor` (主城)。
    
2. **Supervision:** 显式调用 `scene->set_parent_id(room_mgr)`，建立监管树。
    
3. **Logic Load:** 调用 `load_handlers()` 注册 lambda 回调。
    
4. **Runtime Start:** 启动 `Scheduler` 线程池。
    
5. **IO Start:** 启动 `io_uring` 事件循环 (阻塞主线程)。
    

#### B. 会话处理 (The Session Loop)

代码 `handle_session` 展示了 **"One Coroutine Per Connection"** 的精髓：

1. **Physical Wrap:** 将原生 `Socket` 包装为 `Connection`。
    
2. **Actor Binding:** 立即创建一个 `PlayerActor`。此时玩家处于"已连接未登录"状态。
    
3. **Read Loop:**
    
    - `co_await conn->read_packet()`: 挂起协程，等待数据。
        
    - **Route:** 收到包后，封装为 `NetworkMessage`，Push 到 `PlayerActor` 的邮箱。
        
    - **Wakeup:** `Scheduler::dispatch(actor)` 唤醒 Actor 处理。
        
4. **Disconnection:** `read_packet` 返回空 -> Push `SessionClosedMsg` -> Actor 执行清理。
    

### 4. 关键技术决策 (Key Design Decisions)

#### A. 协程并发模型：Stackless Coroutines

- **决策：** 为每个 TCP 连接启动一个 `DetachedTask` (C++20 无栈协程)。
    
- **理由：**
    
    - **内存极省：** 相比 Go 的有栈协程 (2KB+)，无栈协程的 Frame 只有几十字节。单机 10万 连接的内存开销极低。
        
    - **逻辑线性：** 没有 `on_recv` 回调，死循环 `while(true)` 读包，符合人类直觉。
        

#### B. 预先绑定 Actor (Pre-binding)

- **决策：** 在收到第一个包之前，就创建了 `PlayerActor`。
    
- **理由：**
    
    - **统一模型：** 哪怕是第一个 `LoginReq` 包，也是通过 Actor 邮箱处理的，不需要在 Gate 层写特殊的"未登录逻辑"。
        
    - **状态机：** `PlayerActor` 内部维护 `loggedIn` 状态。如果未登录发了移动包，Actor 内部直接丢弃或踢人。
        

#### C. 定时器驱动：TimerFD + Coroutine

- **决策：** `timer_loop` 协程读取 Linux `timerfd`。
    
- **理由：** 将时间事件转化为 IO 事件，统一由 `io_uring` 驱动。协程醒来后执行 `TimeWheel::tick()`，实现了高精度定时。
    

### 5. 关键实现细节 (Implementation Deep Dive)

#### A. 登录流程与 Handler 装配

在 `HandlerLoader` 中：

1. **Auth:** 收到 `CS_LOGIN_REQ`，设置 `PlayerActor` 的业务 `UID`。
    
2. **Spawn:** 计算出生点，`player->SetPos(...)`。
    
3. **Enter:** 找到全局 `g_DefaultSceneID`，发消息进入场景。
    
    这展示了 Gate 如何作为一个无状态的管道，将协议转化为有状态的 Actor 行为。
    

#### B. 断线处理 (Graceful Shutdown)

- Gate 层**不负责**销毁 `PlayerActor`。
    
- Gate 只负责发送 `SessionClosedMsg`。
    
- `PlayerActor` 收到该消息后，负责存盘、从场景移除自己，最后调用 `finalize()` 自杀。
    
- 这保证了业务数据的完整性，避免了直接 `delete actor` 导致的逻辑截断。
    

#### C. 消息转换与零拷贝

在 `handle_session` 中：

```
auto msg = NetworkMessagePool::instance().acquire(std::move(packet), ...);
actor->push(msg.release());
```

- `Packet` 从 `io_uring` 读出来后，直接移交给 `NetworkMessage`，再进入 Actor 邮箱。全过程没有发生 Payload 的内存拷贝。
    

### 6. 总结 (Summary)

`GateServer` 与 `HandlerLoader` 共同构成了 Aegis 的应用层：

- **GateServer** 解决了 **"连接与并发"** (IO Bound)。
    
- **HandlerLoader** 解决了 **"协议与逻辑"** (CPU Bound)。
    

它们通过 **Actor 模型** 完美解耦：Gate 只管生产消息，Actor 只管消费消息，Scheduler 负责调度。