# Aegis Engine: Protocol & Data Contract (通信契约)

### 1. 模块定义 (What & Why)

- **一句话定位：** 这是连接 Client (Unity/C#) 与 Server (C++)，以及 Server 内部各节点通信的 **IDL (接口定义语言)** 规范。
    
- **设计初衷 (Motivation)：**
    
    - **跨语言交互：** C++ 的内存布局 (Memory Layout) 与 C# 不同。我们需要一种平台无关的序列化格式来交换数据。
        
    - **版本兼容性：** 游戏更新频繁，协议必须支持“增量更新”（加新字段不挂旧客户端）。
        
    - **路由基石：** 网络层 (`Net Layer`) 不解析具体内容，仅依靠 `MsgID` 进行 O(1) 的路由分发。
        

### 2. 核心技术决策 (Key Decisions & Trade-offs)

#### A. 序列化方案：Protobuf 3

- **决策：** 使用 Google Protobuf v3。
    
- **方案对比：**
    
    - **vs JSON/XML:** Protobuf 是二进制流，体积小 3-10 倍，解析速度快 20-100 倍。对于每秒同步 30 次的 `CSMoveReq`，文本格式的带宽开销是不可接受的。
        
    - **vs FlatBuffers:** FlatBuffers 零拷贝性能更好，但 API 极其反人类，开发效率低。Protobuf 在性能与易用性之间取得了完美的平衡。
        
- **取舍：** 牺牲了可读性（抓包看到的是二进制），需要配合 Wireshark 插件或 `DebugString()` 进行调试。
    

#### B. 显式 MsgID 映射 (`ids.proto`)

- **决策：** 独立定义 `enum MsgID`，而不是使用 Protobuf 反射的 `GetDescriptor()->fullName()`。
    
- **理由 (面试高频)：**
    
    - **性能：** `switch(int)` 是汇编级的跳转表 (Jump Table)，复杂度 O(1)。字符串哈希查找虽然理论 O(1) 但常数大且有冲突风险。
        
    - **解耦：** 网络底层 (`Packet`) 只需要读前 4 字节的 ID 就能决定投递给哪个 Actor，无需反序列化整个 Body。
        
    - **范围管理：** 通过 `1000-1999` (Lobby), `2000-2999` (Battle) 的段位划分，方便做网关层的权限控制和转发策略。
        

#### C. 命名规范 (Hungarian Notation)

- **决策：** 消息体强制前缀 `CS_` (Client->Server), `SC_` (Server->Client), `SS_` (Server->Server)。
    
- **收益：** 代码审查时，一眼就能看出 `CS_LOGIN_RES` 是个非法命名（客户端不可能给服务器发响应），极大降低了逻辑错误。
    

### 3. 关键实现细节 (Implementation Deep Dive)

#### A. 结构复用 (Composition)

在 `common.proto` 中定义基础类型：

```
message PBVector3 { float x=1; float y=2; float z=3; }
```

在 `cs_battle.proto` 中引用：

```
import "common.proto";
message CSMoveReq { aegis.common.PBVector3 target_pos = 1; }
```

这避免了在每个消息里都重复定义 x,y,z，保证了数据结构的一致性。

#### B. AOI 列表优化

```
message SCEnterViewNtf {
  repeated aegis.common.PBPlayerInfo entities = 1;
}
```

- **内存布局：** Protobuf 的 `repeated` 字段在 C++ 中映射为 `RepeatedPtrField` (类似 `std::vector`)。
    
- **Arena Allocation：** Protobuf 底层使用 Arena 内存分配器。当 AOI 广播大量实体时，它会在一块连续内存上分配对象，对 CPU Cache 非常友好。
    

### 4. 踩坑与难点 (Challenges & Solutions)

#### 难点 1：消息号冲突

- **问题：** 多人协作开发时，两个人同时加了 ID `1005`，合并代码时冲突，或者运行时串包。
    
- **解决：**
    
    - **强制枚举：** 所有 ID 必须在 `ids.proto` 统一管理。
        
    - **自动化生成：** 编写 Python 脚本扫描 `.proto` 文件，自动生成 C++ 的 `Map<MsgID, ParseFunc>` 绑定代码，如果 ID 重复，编译期直接报错。
        

#### 难点 2：浮点数精度

- **问题：** `PBVector3` 使用 `float`。Unity (Client) 和 C++ (Server) 对浮点数的二进制表示虽然都是 IEEE 754，但累积误差可能不同步。
    
- **解决：**
    
    - 对于位置同步，直接传输 `float` 通常够用。
        
    - 但对于核心判定（如碰撞检测），服务端必须拥有“权威逻辑”，客户端的计算仅作预测表现。如果误差超过阈值（如 0.5m），服务端下发 `SCMoveNtf` 强制拉回 (Rubber Banding)。
        

### 5. 性能复杂度 (Complexity)

- **序列化/反序列化：** **O(N)**，N 为字段数量。Protobuf 3 移除了 `unknown fields` 的保留机制，速度极快。
    
- **路由分发：** **O(1)**，纯整数匹配。
    
- **空间开销：** 相比 JSON 节省 60%~80% 的带宽。