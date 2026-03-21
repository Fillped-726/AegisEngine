# Aegis Actor Framework: `Actor` 基类技术文档

## 1. 模块概述

`Actor` 类是 Aegis 框架中最为核心的并发执行单元基座。它实现了一个**无锁（Lock-Free）、线程安全且内存对齐**的消息信箱（Mailbox）与状态机。该基类旨在彻底隔离底层多线程调度与上层业务逻辑，使得继承此基类的派生类（如 `PlayerActor`、`SceneActor`）在处理业务逻辑时，完全无需关注互斥锁（Mutex）与并发数据竞争。

**核心特性：**

- **Intrusive MPSC Queue：** 基于侵入式多生产者单消费者无锁队列，消除节点分配开销。
    
- **Wait-Free Enqueue：** 生产者入队操作为无等待（Wait-Free）级别，极高并发下无锁竞争衰退。
    
- **Cache-Friendly Layout：** 严格的内存物理隔离，防御多核 CPU 伪共享（False Sharing）。
    

---

## 2. 内存模型与底层布局

为了在多核架构下压榨极限吞吐量，`Actor` 类的内存布局进行了极其严格的设计。

C++

```
private:
    static constexpr size_t kCacheLine = hardware_constructive_interference_size;

    alignas(kCacheLine) ActorMessage *head_;                     // Consumer 独占变量
    alignas(kCacheLine) std::atomic<ActorMessage *> tail_;       // Producer/Consumer 共享热点
    alignas(kCacheLine) std::atomic<bool> in_global_queue_;      // 调度状态锁
```

- **False Sharing（伪共享）防御：** 通过 `alignas(kCacheLine)`，强制将读指针（`head_`）、写指针（`tail_`）和调度状态（`in_global_queue_`）分配在不同的物理缓存行（通常为 64 字节）上。
    
- **原理：** 确保多个生产者在竞争修改队尾时，其产生的 Cache Line Invalidation（缓存行失效）不会波及正在独立工作的消费者，从而避免 CPU 缓存颠簸（Cache Bouncing）。
    

---

## 3. 核心数据结构：Stub Node (哑节点) 机制

`Actor` 的信箱底层采用带有**哑节点（Stub Node）**的 MPSC 队列。

- **初始化状态：** 构造函数中会 `new` 一个没有任何实际数据的空 `ActorMessage` 作为初始的 Stub，`head_` 和 `tail_` 同时指向它。
    
- **设计目的：** 确保队列中**永远至少存在一个节点**。这在物理上隔离了生产者的写操作（只修改 `tail` 和队尾的 `next`）与消费者的读操作（只读取 `head->next`），从而规避了空队列状态下首尾指针的 Double-CAS 竞争。
    
- **延迟回收：** 消费者处理完有效节点的数据后，该节点的外壳会成为新的 Stub Node，而旧的 Stub Node 才会被释放。
    

---

## 4. 并发状态机与核心工作流

## 4.1 生产者入队：`push(T *msg)`

允许任意外部线程（IO 线程、其他 Worker 线程）安全调用。

1. **节点初始化 (Relaxed)：** 将传入消息的 `next` 置空。由于此时节点未暴露，使用 `memory_order_relaxed` 即可。
    
2. **原子占位 (Acq_Rel)：** 通过 `tail_.exchange(msg, std::memory_order_acq_rel)` 无等待地将队尾指向新消息，并获取前驱节点（旧队尾）。
    
3. **内存可见性投递 (Release)：** `prev->next.store(msg, std::memory_order_release)`。此处的 Release 语义是数据安全的基石，保证消息载体内的业务数据在指针被连通前，已强制刷入主存，对消费者绝对可见。
    
4. **调度唤醒 (CAS)：** 如果 Actor 原本处于闲置（Idle）状态，利用 `compare_exchange_strong` 原子的将 `in_global_queue_` 设为 `true`，并返回给外层调度器以触发入列。
    

## 4.2 消费者出队：`process_batch(int budget)`

仅允许单一 Worker 线程执行（单消费者约束）。该函数引入了严密的异常防御与竞态抢救机制。

1. **屏障读取 (Acquire)：** 通过 `head->next.load(std::memory_order_acquire)` 获取真实数据节点。
    
2. **Double-Check 竞态防御：**
    
    - 当 `next == nullptr` 时，并非一定代表队列为空。可能存在**微秒级撕裂状态**（生产者已完成 `exchange`，但尚未执行 `next.store`）。
        
    - 此时必须比对 `head` 与 `tail`。若 `head != tail`，说明遇到了“幻影节点”（生产者正在途中）。
        
    - **自救逻辑：** 消费者通过 CAS 重新夺回 `in_global_queue_` 锁，触发 `continue` 进行隐式自旋（Spin-Wait），等待指针连通，彻底杜绝 Actor 假死丢包。
        
3. **毒丸机制 (Poison Pill)：** 拦截 `MSG_TYPE_DESTROY`。遇到该消息直接返回 `ActorState::Dead`，通知底层调度器执行析构。
    

## 4.3 静态分发与垃圾回收：`free_message(ActorMessage *msg)`

针对无虚析构函数的多态消息清理模块。

- **Jump Table 优化：** 摒弃 `std::function` 映射表与动态多态，采用连续的 `switch-case` 判定 `type_id` 并结合 `static_cast` 执行强转。
    
- **性能考量：** 依赖编译器生成静态跳转表（Jump Table），配合 `finalize()` 函数内联，实现极其硬核的 O(1) 释放，消除所有不必要的间接调用与栈帧开销。同时兼顾了网络池化消息的异构回收。
    

---

## 5. 公共 API 参考

|**方法签名**|**线程安全性**|**描述**|
|---|---|---|
|`bool push(T *msg)`|**Thread-Safe** (Lock-Free)|投递消息。若返回 `true`，调用方需负责将该 Actor 压入全局调度队列。|
|`ActorState process_batch(int budget)`|**Non-Thread-Safe** (Worker Only)|批量处理消息。返回 `Idle` (需挂起), `Active` (需重入队), 或 `Dead` (需销毁)。|
|`virtual void finalize() = 0`|-|纯虚函数。供派生类实现特殊的清理逻辑。|
|`static Actor* current()`|Thread-Local|获取当前执行环境所属的 Actor 上下文。|

---

