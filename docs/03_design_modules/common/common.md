# Aegis Engine: Common Module Technical Documentation

> **Status:** Core Infrastructure / Kernel Layer
> 
> **Language:** C++20
> 
> **Key Attributes:** Lock-Free, Zero-Copy, Cache-Friendly

## 1. 模块全景图 (The Big Picture)

### 1.1 定位与目标

**Common 模块** 是 Aegis 引擎的 **Kernel Layer (核心内核层)**。

- **目标：** 提供高性能、零开销的底层原语。
    
- **地位：** 整个引擎的大厦地基，不依赖任何上层业务（如 Network, Actor, Gameplay）。
    

### 1.2 职责边界

|   |   |
|---|---|
|**负责 (In-Scope)**|**不负责 (Out-of-Scope)**|
|**内存管理** (ObjectPool, TLS)|网络 I/O|
|**并发调度** (WorkStealingQueue)|协议解析|
|**基础结构** (IntrusiveList)|业务逻辑调度|
|**线程同步** (SpinLock, Atomics)|游戏玩法逻辑|
|**资源管理** (RAII, UniqueFd)||

### 1.3 依赖关系

- **上游 (Dependents):** Network Layer (Buffer, Connection), Actor System (Message, Mailbox).
    
- **下游 (Dependencies):** OS Kernel (Futex, Epoll, mmap), C++20 Standard Library.
    

## 2. 架构与类图 (Architecture)

### 2.1 核心组件拓扑

1. **Concurrency Group (并发组):**
    
    - `WorkStealingQueue`: 任务调度核心，连接 Producer/Consumer，基于 Chase-Lev 算法。
        
    - `SpinLock`: 用户态自旋锁，配合 `_mm_pause` 保护极短临界区。
        
2. **Memory Group (内存组):**
    
    - `ObjectPool` (Singleton): 全局内存总管，负责兜底分配。
        
    - `ThreadLocalCache` (TLS): 线程私有缓存，实现无锁快速分配。
        
    - `IntrusiveList`: 侵入式链表，对象即节点，零内存分配。
        
3. **Utility Group (工具组):**
    
    - `AegisLog`: 基于 C++20 `std::source_location` 的无宏日志系统。
        
    - `UniqueFd`: 文件描述符的 RAII 包装。
        

### 2.2 类关系图 (Class Diagram)

```
classDiagram
    %% ==========================================
    %% 1. 内存支柱 (Memory Pillar)
    %% ==========================================
    class ObjectPool {
        +GlobalQueue: ConcurrentQueue
        +acquire()
        +release()
        -is_active_: atomic bool
    }
    class ThreadLocalCache {
        +vector~T*~ ptrs
        +bulk_buffer
        +prod_token
        +cons_token
    }
    
    %% 关系：池管理着每个线程的缓存
    ObjectPool "1" *-- "N" ThreadLocalCache : Manages

    %% ==========================================
    %% 2. 调度支柱 (Scheduling Pillar)
    %% ==========================================
    class WorkStealingQueue {
        +push()
        +pop()
        +steal()
        -buffer_: array
        -top_: atomic size_t
        -bottom_: atomic size_t
    }
    %% 注：调度队列是独立的，不需要依赖其他组件

    %% ==========================================
    %% 3. 结构支柱 (Structure Pillar - For Timers/Lists)
    %% ==========================================
    class IntrusiveListNode {
        +prev: Node*
        +next: Node*
        +unlink()
    }
    
    class IntrusiveList {
        +root_: Node
        +push_back()
        +remove()
        +splice()
    }

    %% 关系：链表操作节点
    IntrusiveList o-- IntrusiveListNode : Manipulates

    %% ==========================================
    %% 4. 辅助工具 (Utilities)
    %% ==========================================
    class UniqueFd {
        -fd_: int
        +reset()
    }
    
    class SpinLock {
        -flag: atomic_flag
        +lock()
        +unlock()
    }

    %% 样式调整
    note for ObjectPool "负责内存分配\n(TCMalloc Style)"
    note for WorkStealingQueue "负责任务调度\n(Chase-Lev)"
    note for IntrusiveList "负责对象组织\n(Timer/Event)"
```

### 3. 核心交互流程 (Key Workflows)

**目标：** 展示数据在“时间维度”上的流转。

#### 流程 A：对象的极速分配与回收 (The Zero-Contention Alloc)

这是系统中最频繁的操作（每秒可能发生百万次）。

1. **Request:** 业务线程调用 `ObjectPool::acquire<Packet>()`。
    
2. **L1 Check (Fast Path):** 检查 `ThreadLocalCache` 是否为空？
    
    - _Yes:_ 仅仅移动 `vector` 尾指针 (`pop_back`)，耗时 **~2ns**。 -> **Return Object**.
        
3. **L1 Miss (Slow Path):** 本地缓存空了。
    
    - 调用 `global_queue_.try_dequeue_bulk`。
        
    - 从全局池“批发” 128 个对象填充到本地缓存。
        
    - 返回 1 个对象。
        
4. **Recycle:** 业务用完调用 `ObjectPool::release(ptr)`。
    
    - 放入 `ThreadLocalCache` (`push_back`)。
        
    - **Watermark Check:** 如果本地缓存超过 256 个，触发 **Bulk Release**，将一半对象（128个）归还全局池。
        

#### 流程 B：任务窃取 (The Work Stealing)

这是多核 CPU 负载均衡的核心。

1. **Schedule:** 线程 A 生成任务，调用 `WorkStealingQueue::push` (Bottom 端入队，无锁，Release语义)。
    
2. **Execute:** 线程 A 空闲，调用 `pop` (Bottom 端出队，无锁，Relaxed语义)。
    
3. **Starvation:** 线程 B 空闲，发现自己队列空了。
    
4. **Theft:** 线程 B 扫描线程 A 的队列。
    
    - 调用 `steal` (Top 端出队)。
        
    - **CAS Loop:** 尝试原子修改 Top 索引。
        
    - _Success:_ 拿走任务执行。
        
    - _Conflict:_ 如果线程 A 同时也 pop 最后一个任务，触发 **SeqCst Fence** 仲裁，保证只有一个线程成功。
              

## 4. 关键技术决策 (Key Design Decisions)

|   |   |   |   |
|---|---|---|---|
|**技术点**|**决策理由 (Why)**|**收益 (Benefit)**|**难点/代价**|
|**TCMalloc 内存模型**|避免全局锁竞争，利用线程局部性。|**99% 分配无锁 (Zero Contention)**，避免上下文切换。|需处理线程退出时的内存泄露/归还问题。|
|**Chase-Lev Deque**|优于普通 MPMC 队列，利用 SPMC 特性。|Push/Pop 优化为原子 Store/Load，**消除 CAS 开销**。|极其复杂的 Memory Order (`acquire/release` + `seq_cst`)。|
|**侵入式链表 (Intrusive List)**|避免 `std::list` 的节点内存分配。|**零内存分配**，缓存友好 (Cache Friendly)，迭代器不失效。|侵入式设计，类必须继承 `Node`。|
|**TTAS SpinLock**|针对极短临界区，避免内核态切换。|开销从 Syscall 的 **~1us 降至 ~10ns**。|需配合 `_mm_pause` 防流水线刷新，`yield` 防活锁。|
|**C++20 Zero-Macro Log**|`std::source_location` + `consteval`。|现代化语法，**编译期格式检查**，无宏污染。|需 C++20 支持。|

## 5. 接口示例 (API Usage)

```
// 1. 定义对象 (继承侵入式节点)
struct MyTask : public aegis::common::IntrusiveListNode {
    int id;
    void reset(int new_id) { id = new_id; unlink(); } // Pool 回收时调用
};

// 2. 高频创建 (L1 Cache 命中, ~2ns)
auto task_ptr = aegis::core::ObjectPool<MyTask>::instance().acquire(1001);

// 3. 调度 (Lock-Free Push)
queue.push(task_ptr.get());

// 4. 日志 (C++20, 自动获取行号)
log.info("Task created: id={}, ptr={}", task_ptr->id, (void*)task_ptr.get());
```
