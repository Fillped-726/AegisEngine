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