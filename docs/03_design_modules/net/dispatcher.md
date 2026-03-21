#### 1. 模块定义 (What & Why)

- **一句话定位：** `Dispatcher` 是 Aegis 引擎的网络中枢路由器（Router）。它负责将无类型、无状态的底层二进制流（`Packet`）或内部 RPC 对象指针，精准地反序列化并路由到对应 Actor 的强类型异步处理函数（`Task<void>`）中。
    
- **设计初衷 (Motivation)：** 底层网络通信只认识字节（Bytes），而上层游戏业务逻辑只认识强类型的对象（如 `PlayerMoveReq`）。如果在每个 Actor 内部手写 `switch-case` 和强转，代码会极其臃肿且极易出错。本模块旨在通过**类型擦除（Type Erasure）**和 **C++20 泛型编程**，构建一个不可变的全局路由表，实现网络层与业务逻辑层的彻底解耦。
    

#### 2. 核心技术决策 (Key Decisions & Trade-offs)

- **决策 A：基于闭包的类型擦除 vs 传统的面向对象多态**
    
    - **对比：** 传统做法通常要求所有消息继承自一个基类（如 `IMessage`），并通过虚函数 `handle()` 处理。这里使用了 `std::function` 配合泛型 Lambda 闭包。
        
    - **取舍：** 彻底抛弃了臃肿的类继承体系（OOP）。利用 Lambda 在注册时（Register Phase）就将**具体的反序列化代码**和**业务函数**打包成一个统一签名的闭包（`NetHandler` / `RpcHandler`）。这不仅减少了虚表查找开销，还使业务代码的编写变成了极其清爽的函数式风格。
        
- **决策 B：读写生命周期隔离 (Phase Isolation) 替代 读写锁 (RWLock)**
    
    - **对比：** 在全局单例的 `unordered_map` 上并发路由，通常需要加 `std::shared_mutex` 防止 Data Race。
        
    - **取舍：** **零锁设计。** 严格规范架构契约：所有的 `register_handler` 动作必须在服务器初始化的**单线程预热阶段（Pre-flight Init）**完成。一旦进入高并发的**运行阶段（Runtime）**，路由表即被冻结为**绝对只读（Read-Only）**。牺牲了运行时的动态热插拔注册能力，换取了热路径（Hot Path）上极致的无锁并发查询性能。
        

#### 3. 关键实现细节 (Implementation Deep Dive)

- **C++20 Concepts 编译期防火墙：**
    
    - 引入 `requires ProtobufMessage<ProtoMsg>` 概念约束。如果业务层手误注册了一个非 Protobuf 的普通结构体，编译器会在第一时间（编译期）报错，而不是在运行时因为 `ParseFromArray` 不存在而引发灾难。
        
- **协程栈的变量生命周期魔法 (Variable Lifting)：**
    
    - 在 `NetHandler` 的 Lambda 中，我们在局部作用域定义了栈变量 `ProtoMsg msg`，随后调用 `co_await func(actor, msg)`。
        
    - **硬核亮点：** 在传统回调中，挂起意味着函数返回，栈变量被销毁，导致野指针。但在 C++20 协程中，编译器会自动将 `msg` 提升（Lift）分配到**协程帧（Coroutine Frame）**的堆内存中。当 `co_await` 挂起交出执行权时，`msg` 依然安全存活；当协程被 CQE 唤醒恢复时，数据完好无损。
        
- **零拷贝的 RPC 路由：**
    
    - 相比于网络消息需要 Parse，`register_rpc` 接口直接利用强转 `static_cast<const RpcMsgType *>(msg_ptr)`。因为内部 RPC 消息信任发送方的内存布局，实现了同一进程（或 Actor 系统内）消息传递的绝对零拷贝。
        

#### 4. 踩坑与难点 (Challenges & Solutions)

- **安全防御：防止恶意包引发的下溢出 (Integer Underflow)**
    
    - **场景描述：** 外网环境下，如果黑客伪造一个总长度只有 2 字节（小于 Header 长度 4）的畸形包。
        
    - **问题与解决：** 如果直接使用 `pkt.size() - kPacketMsgHeader`，会触发 `size_t` 的无符号下溢出，变成 `0xFFFFFFFF...`，导致后续的反序列化器越界读取内存崩溃。解决方案是在 `dispatch` 的入口处建立严格的安全边界检查（Security Boundary Check）：`if (pkt.size() < kPacketMsgHeader) co_return;`，坚决不信任任何外部网络的未校验长度。
        
- **面试进阶探讨：从 Hash 查表到 Flat Array 预取优化**
    
    - **深度思考：** 目前基于 `std::unordered_map` 的分发在 L1 Cache 命中率上并不完美（节点分散且有 Hash 计算开销）。
        
    - **升级方案：** 在游戏服务端，协议号（`msg_id`）往往是密集分配的连续整数（如 1000~5000）。在追求极致的网关节点中，我会将其优化为 **Flat Map（扁平化数组 `std::vector<NetHandler>`）**。以 `msg_id` 作为数组的天然索引（Index）。这不仅将时间复杂度降到了极其稳定的 1~2 纳秒级别，其连续的内存布局更能极大触发 CPU 的硬件预取（Hardware Prefetching）机制。
        

#### 5. 性能复杂度 (Complexity)

- **时间复杂度：** 路由查询目前为 **O(1)** (基于 Hash)。优化为 Flat Map 后可达 **O(1) 绝对常数时间**（单纯的数组内存寻址）。
    
- **空间复杂度：** **O(N)**，N 为注册的协议数量。以 2000 个协议为例，哈希表或数组的指针占用仅几十 KB，内存开销可忽略不计。