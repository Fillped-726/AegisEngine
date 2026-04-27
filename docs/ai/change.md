01:
# 📝 Changelog: AegisEngine 底层消息总线重构与新契约引入

**Date:** [2026/4/26] **Scope:** `core/message`, `cs_lobby.proto`

## 🚀 Features (新增契约与协议)

- **[Proto]** 新增 `S2C_KickOut` 协议：用于支持多端登录冲突时的“踢人”下发通知。
    
- **[Proto]** 新增 `S2C_CampSnapshotPush` 协议：用于支持断线重连/顶号接管成功后的全量状态补帧下发。
    
- **[Message]** 新增底层 Actor 调度消息契约：
    
    - `MSG_TYPE_REBIND_CONNECTION` (`RebindConnectionMsg`): 用于跨线程传递新 Socket 句柄，支持底层网络管道的动态热替换。
        
    - `MSG_TYPE_POISON_PILL` (`PoisonPillMsg`): 用于支持断线保留期结束后的“真死”调度与持久化兜底。
        
    - `MSG_TYPE_REQ_SCENE_SNAPSHOT` (`ReqSceneSnapshotMsg`): 用于向 SceneActor 发起内部数据拉取。
        

## 🛠 Refactoring (底层重构与安全升级)

- **[Architecture] 模块化拆分 `message.h`：**
    
    - 彻底消灭了庞大的单体头文件，将其按职责拆分为：`message_id.h` (纯枚举), `message_base.h` (CRTP 基类), `message_lifecycle.h`, `message_net.h`, `message_scene.h`, `message_rpc.h`。
        
    - 保留了伞形头文件（Umbrella Header）`message.h` 以确保向下兼容性。
        
    - 大幅降低了不同模块间的头文件耦合度，优化了整体工程的编译时间。
        
- **[Memory Safety] 突破 256 消息容量限制：**
    
    - 将 `ActorMessage` 和枚举的底层类型从 `uint8_t` 强制升级为 `uint16_t`。
        
    - 将 `g_message_finalizers` 数组安全扩容至 `1024`，彻底消除了未来添加大号消息 ID 时可能引发的内存越界（Buffer Overflow）和数据截断隐患。
        
- **[Bug Fix] 修复致命的头文件循环依赖：**
    
    - 阻断了 `actor.h` -> `message.h` -> `message_scene.h` -> `actor_traits.h` -> `actor.h` 的编译死锁链。
        
    - 统一使用 `actor_registry.h` 替代包含完整的 Traits 文件，恢复了编译器对 `ActorID` 的正常解析。
        
- **[Warning] 消除冗余警告：**
    
    - 清理了 `NpcActor` 构造函数中未使用的参数引起的编译器 Warning。
        

## ⏸️ Deferred (暂缓应用)

- **[业务逻辑] `PlayerActor` 状态机改造暂缓：** 包含 `MSG_TYPE_SESSION_CLOSED` 的挂起逻辑、无锁化改造、以及 `REBIND` 的处理（等待其他前置问题解决后再行合并）。
    
- **[业务逻辑] 网关/登录路由层分发逻辑暂缓：** `handle_login` 拦截旧连接的逻辑尚未应用。

02:
重构总结                                                                                                                                                   
     模块A：剥离业务 — GateServer Purity Refactoring                                                                                                            
                                                                                                                                                                
     | 文件 | 改动 |                                                                                                                                            
     |------|------|                                                                                                                                            
     | services/gate/include/gate_server.h | 剔除 g_DefaultSceneID 静态变量、room_manager_id_ 成员，移除所有 Scene/RoomManager/NPC 相关声明 |                   
     | services/gate/src/gate_server.cpp | 删除所有 #include <SceneActor/NpcActor/RoomManager>、主城创建代码、NPC 刷怪代码、g_DefaultSceneID 引用。init()       
     现在只做：拉起 Log、Scheduler、tune_fd_limit。handle_session 只做：Socket → PlayerActor 绑定 |                                                             
     | services/gate/src/gate_server.h | 接口精简为纯网络层（init / run / handle_session） |                                                                    
     模块B：引入 GameApp — Game Logic Bootstrap                                                                                                                 
                                                                                                                                                                
     | 文件 | 改动 |                                                                                                                                            
     |------|------|                                                                                                                                            
     | include/aegis/core/game_app.h (新) | 单例 GameApp 类。提供 init() 和 default_scene_id() / room_manager_id()  |                                           
     | src/core/game_app.cpp (新) | init() 接管了原来写在 GateServer::init 中的 RoomManager 初始化、默认主城 SceneActor 创建、NPC 刷怪 |                        
     | services/gate/src/main.cpp | 入口流程重构：实例化 GateServer → 实例化 GameApp.init() → server.init() → server.run()。先启动业务域，再启动网络层 |        
     模块C：动态营地创建 — Dynamic Room Routing                                                                                                                 
                                                                                                                                                                
     | 文件 | 改动 |                                                                                                                                            
     |------|------|                                                                                                                                            
     | shared/proto/ids.proto | 新增 4 个 MsgID：C2S_CREATE_CAMP_REQ(1008)、S2C_CREATE_CAMP_RES(1009)、C2S_JOIN_CAMP_REQ(1010)、S2C_JOIN_CAMP_RES(1011) |       
     | shared/proto/cs_lobby.proto | 新增 C2S_CreateCampReq、S2C_CreateCampRes、C2S_JoinCampReq、S2C_JoinCampRes 四个消息 |                                     
     | include/aegis/core/message/message_id.h | 新增 MSG_TYPE_ASSIGN_CAMP(14) — PlayerActor 向 RoomManager 请求营地分配的 Actor 内部消息类型 |                 
     | include/aegis/core/message/message_rpc.h | 新增 AssignCampReq / AssignCampRes 结构体 + RPCAssignCampMsg RPC 消息类型（含                                 
     std::promise<AssignCampRes>）|                                                                                                                             
     | include/aegis/core/room_manager.h | 新增 on_assign_camp() RPC handler 声明 |                                                                             
     | src/core/room_manager.cpp | 实现 on_assign_camp()：根据 is_create 标记决定 创建新 SceneActor 或 查找已存在的营地 SceneActor，返回分配结果 |              
     | services/gate/src/logic/handler_loader.cpp | 注册 3 个处理器：Login（改为从 GameApp 取默认主城）、C2S_CreateCampReq、C2S_JoinCampReq。每个 camp          
     handler 构造 AssignCampReq → dispatch_msg 到 RoomManager → future.get() 等结果 → 构建 S2C 回包 → SceneLeave/SceneEnter 完成场景切换 |                      
     架构变化示意图                                                                                                                                             
                                                                                                                                                                
     Before:                                                                                                                                                    
       main → GateServer.init()                                                                                                                                 
              ├─ Log, Scheduler, tune_fd_limit                                                                                                                  
              ├─ RoomManager 初始化 ✗                                                                                                                           
              ├─ 创建默认主城 SceneActor ✗                                                                                                                      
              ├─ 刷 NPC ✗                                                                                                                                       
              └─ handle_session → PlayerActor (但 gate_server.cpp 处处引用场景逻辑)                                                                             
                                                                                                                                                                
     After:                                                                                                                                                     
       main → GateServer.init()        # 纯网络层                                                                                                               
            → GameApp.init()           # 纯业务域                                                                                                               
              ├─ RoomManager 初始化                                                                                                                             
              ├─ 创建默认主城 SceneActor                                                                                                                        
              └─ 刷 NPC                                                                                                                                         
                                                                                                                                                                
       Login → handler_loader → GameApp::default_scene_id() 取场景                                                                                              
       CreateCamp → handler_loader → RPC → RoomManager::on_assign_camp → 创建新 SceneActor                                                                      
       JoinCamp → handler_loader → RPC → RoomManager::on_assign_camp → 查找已有 SceneActor 

🛠 [Fixed] 核心缺陷修复
1. 客户端：UI 线程同步 IO 阻塞（"全屏卡死"）
症状：点击“开始游戏”后，客户端主循环（Main Loop）失去响应，鼠标转圈。

根因：错误地在 _Process 中调用了 StreamPeerTcp.GetData(4096)。该方法在字节数不足 4096 时会触发硬阻塞（Hard Block），直接挂起 Godot 渲染线程。

修复：重构接收逻辑，改为 Reactive Polling（反应式轮询）。

引入 GetAvailableBytes() 探测缓冲区。

切换为 GetPartialData()，实现“有多少拿多少”的非阻塞读取。

2. 服务端：Actor 调度器非法解引用（"ASan Segfault"）
症状：接收 C2S_CreateCampReq 后服务器发生 SIGSEGV 崩溃，ASan 报错 0x31c0 偏移访问。

根因：僵尸 Actor（Unbound Actor）。RoomManager 被注册但未被调度。当 dispatch_msg 尝试向一个 worker_ == nullptr 的 Actor 投递消息时，触发了对 ConcurrentQueue 内存偏移地址的非法读写。

修复：

在 GameApp::init 中显式为 RoomManager 执行 set_worker_id(worker_id)。

确保全局单例 Actor 拥有合法的“灵魂”（Worker Affinity）。

🚀 [Optimized] 架构演进与优化
1. 通信协议栈决策
现状评估：否决了在实时同步服中引入 gRPC 的方案，避免 HTTP/2 带来的额外 CPU 负载与同步/异步转换的开销。

技术收敛：确立 Custom TLV-Protobuf Stack 为项目核心协议。

Structure: [Length: 4B][MsgID: 4B][Body: Protobuf]。

优势：网关层实现零拷贝（Zero-Copy）路由，无需反序列化即可实现 Session 转发。

2. 协程安全性警告
风险预警：在 handler_loader.cpp 中通过 future.get() 阻塞协程线程被标记为 Critical Risk（死锁隐患）。

重构方向：后续将转向基于 SequenceID 的异步 Request-Response 匹配模型。

🧠 [Evolutionary Update] 免疫基座进化
已将以下规则注入 <Bug_Pathology_Base>，未来将自动预警类似模式：

[Rule-Net-01]: 所有主线程 IO 必须具备 Total-Non-Blocking 特性，严禁硬编码读取长度。

[Rule-Actor-04]: 任何 Actor 在进入 Registry 后、处理第一条消息前，必须具备有效的 Worker 绑定。

 ─  ⚕ Hermes  ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────── 
                                                                                                                                                                                                     
     第一阶段完成总结                                                                                                                                                                                
     已改动的文件                                                                                                                                                                                    
                                                                                                                                                                                                     
     服务端：                                                                                                                                                                                                                                                                                                                                                                              
     | 文件 | 改动内容 |                                                                                                                                                                             
     |------|---------|                                                                                                                                                                              
     | include/aegis/net/packet.h | 加 kAegisMagic、kFrameMagicSize/LengthSize/HeaderSize、kPacketSeqIdSize/MsgIdSize；pack_into 加 seq_id 参数写 [SeqID][MsgID][Body]；新增                         
     seq_id()、set_seq_id()；新增 seq_id_ 字段 |                                                                                                                                                     
     | src/net/packet.cpp | msg_id() 从 data_ + kPacketSeqIdSize 读取；新增 seq_id() 实现；set_seq_id()；copy_from/move_from/reset 同步 seq_id |                                                     
     | include/aegis/net/outbox_batcher.h | prepare_batch 加 Magic + Length 帧头（2 个 iovec）代替原来的单独 Length |                                                                                
     | src/net/connection.cpp | read_packet 读 kFrameHeaderSize=8B，校验 Magic，滑动恢复 |                                                                                                           
     | include/aegis/core/sequence_id.h | 新增：SequenceIDGen，格式 (worker_id << 24) \| counter |                                                                                                   
     | include/aegis/core/playerActor.h | send_packet 加 seq_id 参数；send_buffer 加 seq_id 参数，写 [SeqID][MsgID][Body] |                                                                          
     | include/aegis/core/message/message_net.h | ForwardPacketMsg 加 seq_id 字段和对应构造函数 |                                                                                                    
     | services/gate/src/logic/handler_loader.cpp | 三处 send_packet 调用加 0 作为 seq_id 占位 |                                                                                                     
                                                                                                                                                                                                     
     客户端：                                                                                                                                                                                        
                                                                                                                                                                                                     
     | 文件 | 改动内容 |                                                                                                                                                                             
     |------|---------|                                                                                                                                                                              
     | Foundation/Protocol/AegisProtocol.cs | 新常量（Magic、Frame/Content header）；Pack 改 (msgId, seqId, body)；新增 ReadMagic/ReadSeqId/IsValidMagic |                                           
     | Network/AegisClient.cs | SendMessage 传 seqId=0；TryDispatchPackets 加 Magic 校验 + 滑动恢复；解析 [FrameHeader][SeqID][MsgID][Body] |                                                        
     核心设计决策                                                                                                                                                                                    
                                                                                                                                                                                                     
     - [Magic(4)][Length(4)] 是帧层，由 OutboxBatcher / read_packet 处理                                                                                                                             
     - [SeqID(4)][MsgID(4)][Body] 是包层，由 Packet / Dispatcher 处理                                                                                                                                
     - Magic 不匹配时服务端/客户端都会滑动 1 字节恢复，防止粘包/脏数据                                                                                                                               
     - SeqID 高位 8bit=worker_id，低位 24bit=自增计数器

     ─  ⚕ Hermes  ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────── 
                                                                                                                                                                                                     
     第二阶段完成总结                                                                                                                                                                                
     改动的所有文件                                                                                                                                                                                  
                                                                                                                                                                                                     
     新增文件：                                                                                                                                                                                      
     - include/aegis/core/sequence_id.h — per-worker 原子 SeqID 生成器，格式 (worker_id << 24) | counter                                                                                             
     - include/aegis/core/rpc_awaiter.h — RpcAwaiter(awaitable)、RpcManager(per-worker 挂起表)、RpcCall<>() 便捷函数                                                                                 
                                                                                                                                                                                                     
     修改文件：                                                                                                                                                                                      
                                                                                                                                                                                                     
     | 文件 | 改动 |                                                                                                                                                                                 
     |------|------|                                                                                                                                                                                 
     | packet.h | 新常量 kAegisMagic, kFrameMagicSize/LengthSize/HeaderSize, kPacketSeqIdSize/MsgIdSize；pack_into 加 seq_id 参数；新增 seq_id()/set_seq_id()；新增 seq_id_ 字段 |                   
     | packet.cpp | msg_id() 偏移调整；新增 seq_id()/set_seq_id()；copy_from/move_from/reset 同步 seq_id |                                                                                           
     | outbox_batcher.h | prepare_batch 写 [Magic][Length] 帧头代替原来的单 Length |                                                                                                                 
     | connection.cpp | read_packet 读 8B 帧头 + Magic 校验 + 滑动恢复 |                                                                                                                             
     | connection.h | K_HEADER_SIZE 保留，但已不再使用（由 kFrameHeaderSize 替代） |                                                                                                                 
     | playerActor.h | send_packet/send_buffer 加 seq_id 参数；新增 MSG_TYPE_RPC_RESPONSE 处理分支 |                                                                                                 
     | message/message_net.h | ForwardPacketMsg 加 seq_id 字段 |                                                                                                                                     
     | message/message_rpc.h | 加 RpcId、RpcResponseMsg、RpcReplyMode；Reply() 支持协程模式投递 RpcResponseMsg |                                                                                     
     | message/message_id.h | 新增 MSG_TYPE_RPC_RESPONSE = 70 |                                                                                                                                      
     | handler_loader.cpp | 创建/加入营地 handler 去掉 future.get() 阻塞，改为 co_await RpcCall<>() |                                                                                                
     架构变化示意                                                                                                                                                                                    
                                                                                                                                                                                                     
     Before (阻塞):                                                                                                                                                                                  
       PlayerActor → RPCAssignCampMsg → RoomManager                                                                                                                                                  
                                         ↓                                                                                                                                                           
                                      处理                                                                                                                                                           
                                         ↓                                                                                                                                                           
                                      msg.Reply(res) → promise.set_value(res)                                                                                                                        
                                                         ↓                                                                                                                                           
                                         future.get() ←──┘  ← 阻塞线程！                                                                                                                             
                                                                                                                                                                                                     
     After (非阻塞):                                                                                                                                                                                 
       PlayerActor → RPCAssignCampMsg → RoomManager                                                                                                                                                  
          ↓ (挂起)                        ↓                                                                                                                                                          
          ↓                             处理                                                                                                                                                         
          ↓                               ↓                                                                                                                                                          
          ↓                      msg.Reply(res) [协程模式]                                                                                                                                           
          ↓                               ↓                                                                                                                                                          
          ↓                      RpcResponseMsg → dispatch_msg → PlayerActor                                                                                                                         
          ↓                                                          ↓                                                                                                                               
          ↓                                          RpcManager.consume(rpc_id)                                                                                                                      
          ↓                                                          ↓                                                                                                                               
          └─────────────────────────────────────────── handle.resume() ← 不阻塞！                                                                                                                    
                                                         ↓                                                                                                                                           
                                               await_resume() → 返回结果                                                                                                                             
                                                                                                                                                                                                     
     需要你手动操作                                                                                                                                                                                  
                                                                                                                                                                                                     
     1. 编译服务端 — 进 services/gate/ 目录重新 make/cmake --build                                                                                                                                   
     2. 编译客户端 — 在 Godot 里 Build → Build                                                                                                                                                       
     3. 启动测试 — 服务端 ./gate_server → Godot 运行客户端 → 点"开始游戏
     
     "      
      ─  ⚕ Hermes  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────── 
                                                                                                                                                                                    
     gate 也只链接了 AegisEngine、AegisProto、jemalloc，没有 gRPC。好。                                                                                                             
     修复总结                                                                                                                                                                       
                                                                                                                                                                                    
     三个问题都处理了：                                                                                                                                                             
     A. RpcResponseMsg 继承修复                                                                                                                                                     
     message_rpc.h 中 RpcResponseMsg 改为继承 BasicMessage<RpcResponseMsg, MSG_TYPE_RPC_RESPONSE>，finalize() 不再是 override，通过 g_message_finalizers 注册表正确调用。           
     B. gRPC 残留清理                                                                                                                                                               
     - ss_bridge.proto：删除 service BattleControl { ... }、option go_package、import "common.proto"                                                                                
     - shared/CMakeLists.txt：删除 find_package(gRPC)、gRPC protobuf 生成步骤、gRPC::grpc++ 链接                                                                                    
     C. 客户端不改                                                                                                                                                                  
     ss_bridge.proto 内容变动不影响客户端（客户端不引用它）。                                                                                                                       
                                                                                                                                                                                    
     你现在可以重新编译服务端试试？记得先清理旧的 build 目录（rm -rf out/build/* 或只清 shared 部分），因为 gRPC 的 pb 文件要去掉。

     整体总结：
     ─  ⚕ Hermes  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────── 
                                                                                                                                                                                    
     Changelog — 2026-04-27                                                                                                                                                         
     Overview                                                                                                                                                                       
                                                                                                                                                                                    
     本次重构和新增的主要目标：改造 AegisEngine 服务端与 Godot 客户端的 wire protocol、实现服务端内部 RPC 异步化、废弃 gRPC                                                         
     残留、新增营地列表查询和加入功能、修复房间生命周期问题。                                                                                                                       
     Wire Protocol 变更                                                                                                                                                             
                                                                                                                                                                                    
     新协议格式（服务端 & 客户端同步更新）：                                                                                                                                        
                                                                                                                                                                                    
     [4B Magic: 0x41454753 "AEGS"][4B Length: BigEndian][4B SeqID: BigEndian][4B MsgID: BigEndian][Protobuf Body]                                                                   
                                                                                                                                                                                    
     - Magic：新增 4 字节魔数，用于包完整性校验，不匹配时滑动 1 字节恢复                                                                                                            
     - Length：SeqID(4) + MsgID(4) + Body 长度                                                                                                                                      
     - SeqID：新增 4 字节序列号，用于 RPC 请求-响应配对                                                                                                                             
     - 帧头（Magic + Length）由 OutboxBatcher / read_packet 处理                                                                                                                    
     - 包内容（SeqID + MsgID + Body）由 Packet / Dispatcher 处理                                                                                                                    
     服务端改动                                                                                                                                                                     
     协议层                                                                                                                                                                         
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | include/aegis/net/packet.h | 新增常量 kAegisMagic、kFrameMagicSize/LengthSize/HeaderSize、kPacketSeqIdSize/MsgIdSize；pack_into 加 seq_id 参数；新增                         
     seq_id()、set_seq_id()、seq_id_ 字段 |                                                                                                                                         
     | src/net/packet.cpp | msg_id() 偏移调整；新增 seq_id()/set_seq_id()；copy_from/move_from/reset 同步 seq_id |                                                                  
     | include/aegis/net/outbox_batcher.h | prepare_batch 写 [Magic][Length] 帧头（2 个 iovec）代替原来的单 Length |                                                                
     | src/net/connection.cpp | read_packet 读 8B 帧头 → 校验 Magic → 滑动恢复 |                                                                                                    
     RPC 异步化（替换 blocking future.get()）                                                                                                                                       
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | include/aegis/core/rpc_awaiter.h | 新增 RpcAwaiter<ResT> awaitable + RpcManager(per-worker 挂起表) + RpcCall<>() |                                                           
     | include/aegis/core/message/message_rpc.h | 新增 RpcId、RpcResponseMsg、RpcReplyMode；Reply() 新增协程模式（投递 RpcResponseMsg 到请求方）；RpcMessage 加                     
     requester_id_、set_rpc_meta()、set_rpc_id() |                                                                                                                                  
     | include/aegis/core/message/message_id.h | 新增 MSG_TYPE_RPC_RESPONSE = 70、MSG_TYPE_CAMP_PLAYER_COUNT = 71 |                                                                 
     | include/aegis/core/playerActor.h | send_packet/send_buffer 加 seq_id 参数；新增 MSG_TYPE_RPC_RESPONSE 处理分支；on_session_closed 通知父场景移除自己 |                       
     | include/aegis/core/message/message_net.h | ForwardPacketMsg 加 seq_id 字段 |                                                                                                 
     | services/gate/src/logic/handler_loader.cpp | 创建/加入营地 handler 去掉 future.get() 阻塞，改为 co_await RpcCall<>() |                                                       
     gRPC 清理                                                                                                                                                                      
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | shared/proto/ss_bridge.proto | 删除 service BattleControl、option go_package、import "common.proto" |                                                                        
     | shared/CMakeLists.txt | 删除 find_package(gRPC)、gRPC protobuf 生成步骤、gRPC::grpc++ 链接 |                                                                                 
     营地列表查询 & 加入功能                                                                                                                                                        
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | shared/proto/cs_lobby.proto | 新增 C2S_QueryCampListReq、CampInfo、S2C_QueryCampListRes |                                                                                    
     | shared/proto/ids.proto | 新增 C2S_QUERY_CAMP_LIST_REQ = 1106、S2C_QUERY_CAMP_LIST_RES = 1107 |                                                                               
     | include/aegis/core/room_manager.h | 新增 CampMeta 结构体、CampPlayerCountMsg 消息类、camp_metas_ 缓存、on_camp_player_count() |                                              
     | src/core/room_manager.cpp | on_assign_camp 创建营地时写入 camp_metas_；加入营地时校验满员（ret_code=4）；on_camp_player_count 人数 0 时销毁 SceneActor |                     
     | services/gate/src/logic/handler_loader.cpp | 新增 Handler 7: C2S_QueryCampListReq → 读 RoomManager 缓存 → 返回列表 |                                                         
     房间生命周期                                                                                                                                                                   
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | src/core/scene_actor.cpp | OnTick 每 60 Tick (~3s) 发 CampPlayerCountMsg 上报人数给 RoomManager |                                                                            
     | include/aegis/core/scene_actor.h | 加 report_counter_ 字段 |                                                                                                                 
     | include/aegis/core/playerActor.h | on_session_closed 发 SceneLeaveMsg 给父场景，断连时自动减人 |                                                                             
     新增文件                                                                                                                                                                       
                                                                                                                                                                                    
     | 文件 | 说明 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | include/aegis/core/sequence_id.h | per-worker 原子 SeqID 生成器，格式 (worker_id << 24) \| counter |                                                                         
     | include/aegis/core/rpc_awaiter.h | RPC 协程 awaitable + per-worker 挂起表 |                                                                                                  
     客户端改动                                                                                                                                                                     
     协议层同步                                                                                                                                                                     
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | Foundation/Protocol/AegisProtocol.cs | 新增 Magic/Frame header/Content header 常量；Pack 改 (msgId, seqId, body)；新增 ReadMagic/ReadSeqId/IsValidMagic |                    
     | Network/AegisClient.cs | SendMessage 传 seqId=0；TryDispatchPackets 加 Magic 校验 + 滑动恢复；解析 [FrameHeader][SeqID][MsgID][Body] |                                       
     Proto 同步                                                                                                                                                                     
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | Proto/CsLobby.cs | 尾部追加 CampInfo、C2S_QueryCampListReq、S2C_QueryCampListRes 的完整 IMessage<> 实现 |                                                                    
     | Proto/Ids.cs | 新增 C2SQueryCampListReq = 1106、S2CQueryCampListRes = 1107 |                                                                                                 
     GameManager                                                                                                                                                                    
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | src/core/GameManager.cs | 新增 SendQueryCampList()、OnCampListReceived 事件、HandleQueryCampListRes()；OnClientConnected 不再自动登录（由 UI 控制）|                         
     MainMenu 改造                                                                                                                                                                  
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | src/ui/MainMenu.cs | 欢迎界面改 "创建营地" + "加入营地" 双按钮；新增创建营地对话框（名称输入 + 保存/取消）；新增加入营地面板（营地列表 + 防抖刷新 +                          
     加载/错误状态）；_Ready 时自动 TCP 连接但不登录；登录由"创建"或"加入"触发 |                                                                                                    
     其他                                                                                                                                                                           
                                                                                                                                                                                    
     | 文件 | 改动 |                                                                                                                                                                
     |------|------|                                                                                                                                                                
     | src/entities/CampPlayer.cs | 修复 Initialize 空引用（改为延迟初始化 + ApplyPendingInit）|                                                                                    
     | src/ui/CampScene.cs | OnMoveTick 加位置去重（变化 > 1 单位才发 SendMove），消除无移动时的疯狂同步 |                                                                          
     | project.godot | 移除 Main 的 AutoLoad |                                                                                                                                      
     已知问题（待后续处理）                                                                                                                                                         
                                                                                                                                                                                    
     1. 移动同步 — 远程玩家位置只在进入视野时同步一次，不会持续更新（缺少 Lerp 插值和同步管理器 Tick 广播）                                                                         
     2. 多人操作面板 — 营地内暂无 UI 显示在线玩家列表/操作                                                                                                                          
     测试验证要点                                                                                                                                                                   
                                                                                                                                                                                    
     1. 双客户端联机：A 创建营地 → B 查询列表 → B 加入 → 两人同场景                                                                                                                 
     2. 离开场景：A 点退出 → B 还在场景 → A 场景 3 秒后销毁                                                                                                                         
     3. 营地列表刷新：防抖 3 秒、满员营地不显示"加入"按钮                                                                                                                           
     4. 协议兼容性：旧客户端无法连新服务端（Magic 校验失败）                                                                                                                      