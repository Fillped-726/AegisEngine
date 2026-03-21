# Aegis Engine: Phase 5 - Services & Protocols (应用服务层)

### 1. 模块全景图 (The Big Picture)

**目标：** 提供基于 Aegis 引擎构建实际游戏服务的**参考实现 (Reference Implementation)** 与 **通信契约 (IDL)**。

- **定位：** **Application Layer (应用层/接入层)**。
    
    - 它是整个技术栈的顶层，是“引擎”到“游戏”的最后一步。
        
    - 它直接通过 `main` 函数启动，将底层所有模块（Net, Core, Actor）组装并运行起来。
        
- **职责边界：**
    
    - **负责 (Infrastructure)：** 进程启动与自检 (Bootstrap)、全服单例 Actor (如 `RoomManager`) 的生命周期管理、TCP 连接的持有与心跳维护。
        
    - **负责 (Logic Binding)：** 定义 `Protobuf` 协议，并将网络消息 ID 映射到具体的 C++ Lambda 业务逻辑。
        
    - **不负责：** 具体的玩法算法（这是 Phase 4 `SceneActor` 的工作）、底层的字节流读写（这是 Phase 2 `Net` 的工作）。
        
- **依赖关系：**
    
    - **Downstream (依赖):** 深度依赖 `Core::Scheduler` (驱动力), `Net::Acceptor` (接入能力), `Actor::Registry` (状态管理)。
        
    - **Upstream (被依赖):** 客户端 (Client) 直接连接此层；外部 HTTP/GM 工具调用此层接口。
        

### 2. 架构与类图 (Architecture & Topology)

**目标：** 展示“启动器”、“协议”与“逻辑装配”的协作关系。

#### 核心组件

1. **Bootstrapper:** `GateServer`。系统的总入口，负责按依赖顺序初始化各个子系统。
    
2. **Contract:** `Protobuf Files` (`ids.pb.h`, `cs_battle.pb.h`)。定义了 Client 与 Server 的通用语言。
    
3. **Binder:** `HandlerLoader`。将静态的协议 (`MsgID`) 绑定到动态的 Actor 行为上。
    
4. **Session:** `ClientSession` (协程)。每个连接独占一个轻量级协程，维护连接上下文。
    

#### 逻辑拓扑图 (Mermaid)

```
classDiagram
    %% 入口与启动
    class GateServer {
        +init(config)
        +run(port)
        -room_manager_id: ActorID
        -accept_loop() Task
        -handle_session(Socket) Task
    }

    %% 业务装配
    class HandlerLoader {
        +load_handlers()
        -register_login()
        -register_move()
    }

    %% 协议定义
    class Protobuf {
        <<IDL>>
        +MsgID (Enum)
        +CSLoginReq
        +SCMoveNtf
    }

    %% 关联关系
    GateServer ..> HandlerLoader : 1. Calls Load
    GateServer ..> Protobuf : 2. Uses IDs
    GateServer *-- RoomManager : 3. Owns Supervisor
    HandlerLoader ..> NetDispatcher : 4. Registers Lambdas
    HandlerLoader ..> PlayerActor : 5. Manipulates
```

#### 数据流向 (Data Pipeline)

- **Inbound:** `Socket (Raw Bytes)` -> `Packet (Framed)` -> `NetworkMessage (Actor Msg)` -> `Dispatcher (Route)` -> `Lambda (Logic)` -> `PlayerActor (State Update)`.
    
- **Outbound:** `Logic` -> `Protobuf Struct` -> `Packet` -> `Outbox (Batching)` -> `io_uring (SQE)` -> `NIC`.
    

### 3. 核心交互流程 (Key Workflows)

**目标：** 展示系统从启动到处理业务的全生命周期。

#### 流程 A：系统启动 (Bootstrap Sequence)

这是面试中常问的 **"服务器 main 函数里都做了什么？"**：

1. **Log Init:** 初始化 `spdlog`，设置日志级别。
    
2. **World Init:** 创建 `RoomManager` (上帝 Actor)，并由它创建默认 `SceneActor` (主城)。
    
3. **Supervision:** 建立 `Scene -> RoomManager` 的父子监管关系。
    
4. **Logic Load:** `HandlerLoader` 注册所有业务 Lambda。
    
5. **Runtime Start:** 启动 `Scheduler` 线程池 (CPU)。
    
6. **IO Start:** 启动 `GateServer::run` 进入 `io_uring` 循环 (IO)。
    

#### 流程 B：会话生命周期 (One Coroutine Per Connection)

展示 C++20 协程如何简化网络编程：

1. **Accept:** `accept_loop` 收到新连接，Spawn 一个 `handle_session` 协程。
    
2. **Bind:** 协程立即创建 `PlayerActor`，建立 `Connection <-> Actor` 的 1:1 绑定。
    
3. **Loop:**
    
    - `co_await conn->read_packet()` (挂起等待 IO)。
        
    - 收到包 -> 封装为消息 -> `actor->push()` -> `Scheduler::dispatch()` (唤醒 Actor)。
        
4. **Close:** 读到 EOF -> 发送 `SessionClosedMsg` 通知 Actor -> Actor 执行存盘与自杀 -> 协程退出。
    

### 4. 关键技术决策与取舍 (Key Design Decisions)

**目标：** 体现架构设计能力。

#### A. 协议选型：Protobuf 3 + Explicit MsgID

- **决策：** 使用 Protobuf 3 进行序列化，并强制定义 `ids.proto` 枚举所有消息 ID。
    
- **理由：**
    
    - **Schema Evolution:** 游戏协议变更频繁，PB 的向前兼容性（新增字段不破坏旧客户端）至关重要。
        
    - **O(1) Routing:** 网络层不解析 Body，仅通过 Header 里的 `MsgID (int)` 进行 `switch-case` 分发，比字符串反射快一个数量级。
        
    - **Payload Separation:** 实现了网络底层与业务逻辑的解耦。
        

#### B. 网关模型：Stateful Gate (有状态网关)

- **决策：** Gate 进程直接持有 `PlayerActor` 和业务逻辑，而不是仅仅作为纯转发代理（Proxy）。
    
- **对比：**
    
    - **Pure Proxy:** 适合超大规模微服务，Gate 只负责转发给后端的 Logic Server。缺点是多一次内网 RPC 跳转，延迟增加。
        
    - **Stateful Gate (Aegis):** 适合追求极致延迟的动作/MOBA 游戏。逻辑直接在网关进程的 Actor 中跑，**Zero-RPC**，延迟最低。
        
- **取舍：** 单进程负载较高，但通过 `Actor` 模型充分利用了多核，适合单服架构。
    

#### C. 业务装配：Lambda Binding

- **决策：** 使用 `Dispatcher::register_handler<MsgType>(lambda)`。
    
- **理由：**
    
    - **Type Safety:** 模板自动推导 Protobuf 类型，编译期保证类型安全。
        
    - **Locality:** 将消息定义与处理逻辑放在一起，避免了传统 switch-case 的代码分散问题。
        

### 5. 对外接口与用法 (API & Usage)

**目标：** 开发者手册 - 如何在这个框架上写一个新的游戏功能？

#### 1. 定义协议 (`.proto`)

在 `cs_battle.proto` 和 `ids.proto` 中添加：

```
// 1. 定义消息体
message CSSkillCastReq { int32 skill_id = 1; }
// 2. 分配 MsgID
enum MsgID { CS_SKILL_CAST = 2007; }
```

#### 2. 注册逻辑 (`handler_loader.cpp`)

```
// 3. 编写处理函数
dispatcher.register_handler<CSSkillCastReq>(
    ids::CS_SKILL_CAST,
    [](Actor* actor, const CSSkillCastReq& req) -> Task<void> {
        auto player = static_cast<PlayerActor*>(actor);
        // 写业务逻辑...
        Log::info("Cast Skill: {}", req.skill_id());
        co_return;
    }
);
```

#### 3. 启动服务 (`main.cpp`)

```
int main() {
    aegis::gate::GateServer server;
    server.init("config.json");
    server.run(8888); // 监听端口
    return 0;
}
```