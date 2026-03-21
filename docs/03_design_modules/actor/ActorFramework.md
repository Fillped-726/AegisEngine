# Aegis Engine: Phase 4 - High Performance Actor Framework

### 1. 模块全景图 (The Big Picture)

**目标：** 提供一个**全异步、无锁、位置透明**的业务逻辑执行环境，将复杂的 MMO 交互逻辑解耦为一个个独立的 Actor 实体。

- **定位：** **Application Logic Layer (业务逻辑层)**。
    
    - 它是 Aegis 引擎的最上层，直接承载游戏玩法（Gameplay）。
        
    - 它运行在 **Core Runtime** 提供的 M:N 调度器之上。
        
- **职责边界：**
    
    - **负责：** 实体状态管理、消息投递与处理、空间视野计算 (AOI)、生命周期监管 (Supervision)。
        
    - **不负责：** 底层网络 IO（由 `Net Layer` 负责，Actor 只拿解包后的对象）、线程调度（由 `Core Layer` 负责）。
        
- **依赖关系：**
    
    - **Downstream (依赖):** `Core::Scheduler` (驱动 Actor 运行), `Common::ObjectPool` (内存分配), `Net::Connection` (发包通道)。
        
    - **Upstream (被依赖):** 具体游戏逻辑 (如 SkillSystem, InventorySystem)。
        

### 2. 架构与类图 (Architecture & Topology)

**目标：** 展示“户籍管理”、“通信机制”与“业务实体”的三角关系。

#### 核心架构组件

1. **Container (容器):** `ActorRegistry`。全局单例，负责 ID 分配与指针映射，是系统的“DNS 服务器”。
    
2. **Base (基座):** `Actor`。包含 `Mailbox` (MPSC 队列) 和 `State` (Active/Idle/Dead)。
    
3. **Payload (载体):** `ActorMessage`。基于 CRTP 的多态消息包。
    
4. **Impl (实现):**
    
    - `PlayerActor`: 玩家代理，持有 `Connection`。
        
    - `SceneActor`: 空间容器，持有 `AOIGrid`。
        
    - `RoomManager`: 监管者 (Supervisor)，管理 Scene。
        

#### 逻辑拓扑图 (Mermaid)

```
classDiagram
    %% 核心管理
    class ActorRegistry {
        +create_actor() ActorID
        +get(ActorID) Actor*
        -slots: FixedArray
        -versions: Array
    }

    %% 消息定义
    class ActorMessage {
        +next: AtomicPtr
        +type_id: uint8
    }

    %% Actor 体系
    class Actor {
        -mailbox: MPSC_Queue
        -status: AtomicState
        +push(msg)
        +process_batch()
    }

    class PlayerActor {
        -conn: Connection
        -pos: AtomicFloat
    }

    class SceneActor {
        -aoi: AOIGrid
        -players: Map
    }

    class RoomManager {
        -rooms: Map
    }

    %% 关系
    ActorRegistry "1" o-- "N" Actor : Manages
    Actor <|-- PlayerActor
    Actor <|-- SceneActor
    Actor <|-- RoomManager
    SceneActor "1" *-- "1" AOIGrid : Owns
    ActorMessage --* Actor : Queued In
```

### 3. 核心交互流程 (Key Workflows)

**目标：** 展示系统如何在无锁的情况下流转数据。

#### 流程 A：消息投递与调度 (The Mailbox Cycle)

这是 Actor 模型的心跳。

1. **Produce:** 任意线程调用 `actor->push(msg)`。
    
    - 使用 `atomic exchange` 将消息挂入 MPSC 队列。
        
    - **CAS 状态机:** 如果 Actor 处于 `Idle` 状态，将其状态改为 `Active` 并推入 `Scheduler` 的全局/本地队列。
        
2. **Consume:** 调度器分配一个 Worker 线程执行 `actor->process_batch()`。
    
    - 批量取出消息 (Batch Processing)。
        
    - 调用 `handle_message(msg)` 执行业务逻辑。
        
3. **Retire:**
    
    - 如果队列空了，Actor 标记为 `Idle`，放弃 CPU。
        
    - 如果还有数据但时间片到了，Actor 保持 `Active`，重新入队等待下一轮调度 (避免饿死)。
        

#### 流程 B：移动与视野广播 (The AOI Flow)

展示 SceneActor 如何在单线程内处理复杂的 O(N^2) 问题。

1. **Input:** `PlayerActor` 收到移动包，更新自身原子坐标，向 `SceneActor` 发送 `SCENE_MOVE` 消息。
    
2. **Calculation:** `SceneActor` 处理消息：
    
    - 调用 `AOIGrid::Move(old, new)`。
        
    - 计算出 `Diff`: `EnterList` (新邻居), `LeaveList` (旧邻居)。
        
3. **Broadcast:**
    
    - 对 `EnterList`: 互相发送 `SC_ENTER_VIEW` (带外观数据)。
        
    - 对 `LeaveList`: 互相发送 `SC_LEAVE_VIEW` (仅 ID)。
        
    - 对 `RemainingNeighbors`: 广播 `SC_MOVE`。
        
4. **Zero-Lock:** 全程无锁，因为 `SceneActor` 保证串行执行。
    

### 4. 关键技术决策与取舍 (Key Design Decisions)

**目标：** 面试核心得分点。

#### A. 内存安全：Generational Index (代际索引)

- **问题：** Actor 销毁后内存被复用，旧的 `ActorID` 可能会错误地指向新对象 (ABA 问题)。
    
- **决策：** `ActorID = Index (32bit) + Version (32bit)`。
    
- **机制：** `Registry` 在槽位复用时自动增加 Version。`get(id)` 时校验 Version 是否匹配。
    
- **收益：** 实现了 O(1) 的安全查找，彻底杜绝悬垂指针风险，且无需使用昂贵的 `std::shared_ptr`。
    

#### B. 并发模型：Share Nothing & MPSC

- **问题：** 多线程游戏逻辑中的锁竞争是性能杀手。
    
- **决策：** Actor 之间**不共享内存**，只通过消息通信。内部使用 **Intrusive MPSC Queue**。
    
- **收益：** 业务逻辑编写完全不需要加锁 (`std::mutex` 被彻底移除)，极大降低了死锁风险和上下文切换开销。
    

#### C. 消息多态：CRTP vs Virtual

- **问题：** 每秒百万级消息对象的虚函数开销。
    
- **决策：** 使用 **CRTP (静态多态)** 实现 `finalize()`，消息类无虚析构函数。
    
- **收益：** 节省了 `vptr` (8字节/对象) 内存，避免了虚表查找，提升了 CPU 分支预测效率。
    

#### D. 空间索引：Grid vs QuadTree

- **决策：** 使用 **扁平化一维数组** 模拟二维网格。
    
- **收益：** 相比四叉树，网格查询是绝对的 O(1)。相比二维数组，扁平化一维数组内存连续，极度 **Cache Friendly**。
    

### 5. 对外接口与用法 (API & Usage)

**目标：** 开发者手册。

#### 1. 定义消息

```
// 继承自 BasicMessage，自动处理内存回收
struct MyMsg : public BasicMessage<MyMsg, 101> {
    int data;
    MyMsg(int d) : data(d) {}
};
```

#### 2. 定义 Actor

```
class MyActor : public PooledActor<MyActor> {
    void handle_message(ActorMessage* msg) override {
        if (msg->type_id == 101) {
            auto* m = static_cast<MyMsg*>(msg);
            LOG("Data: {}", m->data);
        }
    }
};
```

#### 3. 创建与调用

```
// 1. 创建 (返回 ID)
ActorID id = ActorRegistry::instance().create_actor<MyActor>();

// 2. 发送消息 (线程安全)
Actor* actor = ActorRegistry::instance().get(id);
if (actor) {
    actor->push(new MyMsg(42));
}

// 3. 销毁 (异步)
actor->push(new ActorDestroyMsg());
```

#### 4. 监管 (Supervision)

```
// 在创建子 Actor 后
child->set_parent_id(supervisor_id);
// Supervisor 需处理 MSG_TYPE_ACTOR_DIED 消息
```