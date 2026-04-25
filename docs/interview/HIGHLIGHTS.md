# AegisEngine 项目亮点速查

> 用于面试中快速引导话题。

## 10 个可聊亮点

| # | 亮点 | 一句话说明 | 涉及文件 |
|---|------|------------|----------|
| 1 | **io_uring + C++20 协程** | 全异步 I/O，SQ/CQ 共享环形缓冲区消除 syscall 开销 | socket.h, awaiter.h |
| 2 | **64 位 ActorID** | 32-bit index + 32-bit version，O(1) 锁查找，防 ABA | actor_registry.h |
| 3 | **三层对象池** | TLS Batch → 全局池，包和 Actor 零碎片分配 | objectPool.h |
| 4 | **Packet SBO** | 1024B 栈内缓冲，小包零堆分配 | packet.h |
| 5 | **分层时间轮** | 5 级级联，O(1) add/tick/cancel，侵入式链表零拷贝级联 | hierarchy_timer.h |
| 6 | **OutboxBatcher writev** | 64 包一次 writev，零拷贝 scatter-gather，部分写入容错 | outbox_batcher.h |
| 7 | **AOI 网格** | 坐标→网格 O(1)，9 宫格查询，脏标记驱动 | aoi_grid.h, sync_manager.h |
| 8 | **共享 Buffer 广播** | protobuf 一次序列化，shared_ptr 跨线程传递 | sync_manager.h, playerActor.h |
| 9 | **AOI 驱动的 AI 休眠** | 无玩家时不跑 NPC 行为树，CPU 省 90% | npc_actor.h |
| 10 | **CRTP Actor 策略** | SimpleActor vs PooledActor 编译期选择，零虚函数开销 | actor_traits.h |

## 面试话术引导

**如果你想让面试官深入某个话题：**

- "我们花了最多时间在**协程和 io_uring 的集成**上..." → 引导聊 async I/O
- "项目中最巧妙的设计是**ActorID 的版本号机制**..." → 引导聊并发安全
- "性能上最大的优化是**共享 Buffer 广播**..." → 引导聊状态同步方案
- "我们使用**侵入式链表而不是 std::list**..." → 引导聊 C++ 底层优化意识
- "这个项目的一大设计原则是**编译期消除运行时开销**..." → 引导聊模板元编程

## 如果被问到"你学到了什么"：

1. 异步编程模型的演进：从回调地狱到协程线性代码
2. 性能优化的根本是**减少系统调用和内存分配**，不是优化循环
3. Actor 模型的本质是**单线程内的串行化和跨线程的消息传递**
4. 好的抽象层隔离 IO、网络、业务——SyncManager 不需要知道 Connection
