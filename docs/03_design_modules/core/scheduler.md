#### 1. 模块定义 (What & Why)

- **一句话定位：** `Scheduler` 是 Aegis 引擎的 CPU 算力调度中心，基于 **Chase-Lev 工作窃取算法** 实现了多核 CPU 的负载均衡。
    
- **设计初衷 (Motivation)：**
    
    - **解决尾延迟 (Tail Latency)：** 防止某个核心任务堆积，而其他核心空闲。
        
    - **最大化缓存命中率：** 本地队列采用 LIFO（后进先出）模式，保证刚运行过的 Actor（热数据）能被立即再次执行。
        
    - **隔离性：** 为 Actor 提供“沙盒”运行环境，捕获异常，防止单点故障导致整个进程崩溃。
        

#### 2. 核心技术决策 (Key Decisions & Trade-offs)

**Q: 为什么选择 Work Stealing (工作窃取) 而不是 Work Sharing (工作共享)？**

- **决策：** 每个线程拥有独立的无锁队列 (`local_queues_`)，只有在自己空闲时才去“偷”别人的任务。
    
- **对比：**
    
    - **Work Sharing (全局队列)：** 也就是简单的 Thread Pool。所有线程争抢一把大锁，竞争激烈，Cache 颠簸严重。
        
    - **Work Stealing：** 90% 的时间线程只访问自己的本地队列（无锁、无竞争）。
        
- **收益：** 随着核心数增加，性能线性增长，几乎没有扩展性瓶颈。
    

**Q: 为什么本地 Pop 用 LIFO，而 Steal 用 FIFO？**

- **决策：** `try_local_pop` 是从队列尾部（Bottom）拿任务（LIFO），`try_steal` 是从别人队列头部（Top）偷任务（FIFO）。
    
- **原理：**
    
    - **LIFO (Local):** 刚被调度的 Actor 数据很可能还在 L1/L2 Cache 中，立即执行它能极大减少 Cache Miss。
        
    - **FIFO (Steal):** 偷窃操作通常开销较大且容易冲突，从“冷”的一端（Top）偷，既减少了与 Owner 线程在 Bottom 端的竞争（减少 CAS 失败率），又保证了任务调度的公平性（防止饿死老任务）。
        

**Q: 为什么使用 Xorshift32 作为随机数生成器？**

- **决策：** 使用 `fast_rand` (Xorshift32) 替代 `std::mt19937` 或 `rand()`。
    
- **取舍：** 牺牲了随机数的密码学安全性（Engine 内部调度不需要安全随机），换取了 **3ns** 的极速生成能力。标准库的 `mt19937` 状态过大且初始化慢，不适合高频调用。
    

#### 3. 关键实现细节 (Implementation Deep Dive)

- **混合等待策略 (Hybrid Waiting Strategy)：**
    
    - 代码位置：`worker_entry` 中的 `MAX_SPINS` 循环。
        
    - **阶段 1：** **无锁自旋 (Busy Spin)**。尝试 `pop` -> 失败 -> `pause` 指令。
        
        - _技巧：_ 使用 `_mm_pause()` 提示 CPU 流水线这是自旋循环，降低功耗并避免错误的分支预测。
            
    - **阶段 2：** **内核挂起 (Sleep)**。自旋 4000 次后仍无任务，使用 `condition_variable` 挂起。
        
    - **收益：** 兼顾了低负载时的 CPU 节能（不空转）和高负载时的极低延迟（不陷入内核态）。
        
- **Actor 的生命周期与遗言机制：**
    
    - 代码位置：`execute_actor` -> `switch(Dead)`。
        
    - **逻辑：** Actor 死亡时，不仅从 Registry 移除，还负责查找 Parent 并发送 `ActorDiedMsg`。
        
    - **亮点：** 你正确处理了 `ActorID` 的版本号校验。如果父 Actor 已经重生（ID 复用但 Version 变了），遗言不会错误投递，防止了逻辑错乱。
        

#### 4. 踩坑与难点 (Challenges & Solutions) —— **面试必问**

**【致命缺失：IO 驱动在哪里？】**

你现在的 `Scheduler` 是一个纯粹的 **CPU 密集型调度器**。Worker Loop 中只有：

1. `try_local_pop`
    
2. `try_global_pop`
    
3. `try_steal`
    

**问题：** `io_uring` 的 `peek_cqe`（检查网络包是否到达）在哪里执行？

- 如果网络层有单独的 IO 线程：会有跨线程投递的开销（Context Switch）。
    
- 如果没有 IO 线程：**你的服务器永远收不到包，或者只能靠某种魔法唤醒。**
    

**解决方案 (Action Item)：**

需要在 `worker_entry` 的循环中，或者专门的 IO 轮询线程中集成 `io_uring` 的处理。

**最佳实践（One Loop Per Thread）：**

```
// 在 worker_entry 的 while(running) 循环里：
if (id == 0) { // 只有 0 号线程或者特定线程负责 IO，或者每个线程都负责
    io_context.poll(); // 处理 io_uring completion events
}
// 处理完 IO 事件后，它们会把 Task push 到 queue 中，然后下面继续执行
execute_actor(task);
```

_这通常是高性能网关（如 Envoy/Nginx）和游戏网关的核心差异点，面试官会盯着这个问。_

**【惊群效应 (Thundering Herd)】**

- **代码现状：** `dispatch` -> `notify_one_worker`。
    
- **潜在风险：** 虽然你用了 `notify_one` 避免了惊群，但在高并发下，如果每次 `push` 都触发 syscall `notify`，开销依然很大。
    
- **优化技巧：** 只有当 `sleeping_workers_ > 0` 且 `local_queues_` 确实空了时才 notify。目前的实现已经加了 `sleeping_workers_` 检查，这是很好的优化。
    

#### 5. 性能复杂度

- **时间复杂度：**
    
    - `dispatch` (Push): O(1) - 主要是无锁队列入队。
        
    - `execute` (Pop): 本地 O(1)，窃取 O(1) 但常数项较大（因为要 CAS 循环）。
        
- **扩展性：** 理论上支持 64 核甚至 128 核线性扩展，因为极少触碰全局锁 `sleep_mtx`。