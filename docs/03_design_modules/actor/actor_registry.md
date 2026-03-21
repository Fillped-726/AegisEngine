# Aegis Engine 核心架构：ActorRegistry (全局无锁路由表)

## 1. 架构定位 (Architecture Positioning)

**一句话定义：** `ActorRegistry` 是 Aegis 引擎的**寻址中枢 (Global Routing Table)**。它提供纯 Wait-free / Lock-free 的 O(1) 路由查询，核心职责是将逻辑身份（`ActorID`）动态绑定到物理内存地址（`Actor*`）。

**核心架构哲学：逻辑与物理的绝对解耦**

在异步并发系统中，实体之间直接持有物理指针 (`Actor*`) 会引发致命的 TOCTOU (Time-of-Check to Time-of-Use) 漏洞和野指针崩溃。

本组件强制所有模块（网络层、定时器、其他 Actor）仅持有 64 位纯数据 `ActorID`。不仅斩断了对象间的强耦合，更配合底层的**对象池 (Object Pool)** 和 **延迟调度器 (Scheduler)**，从根本上消除了多线程环境下的内存复用 (ABA) 风险与静默污染。

## 2. 核心技术决策与权衡 (Key Decisions & Trade-offs)

### A. 存储结构：固定原子数组 vs 并发哈希表

- **决策**：采用 `std::array<std::atomic<Actor*>, MAX_ACTORS>` 与无锁队列组合，彻底摒弃 `std::unordered_map + shared_mutex` 或 `ConcurrentHashMap`。
    
- **权衡分析**：
    
    - **摒弃哈希**：哈希表存在计算开销、哈希冲突链表遍历开销，最致命的是扩容 (Rehash) 时会导致全局锁引发的 Stop-The-World 抖动。
        
    - **极致 O(1) 与缓存友好**：固定数组提供了真正的 O(1) 内存偏移寻址。底层数组内存连续，对于每秒百万次级的高频路由，CPU Cache 预取命中率极高。
        
    - **容量妥协**：以少量内存（百万容量仅需几十 MB）换取绝对的无锁极速，完全契合单服网关或游戏服的峰值预估模型。
        

### B. 身份验证机制：Generational Index (代际索引)

- **决策**：`ActorID` = `32-bit Index` + `32-bit Version` (物理内存别名映射为 `uint64_t raw`)。
    
- **解决 ABA 问题**：
    
    - 由于内存由对象池复用，当 Actor 销毁时，其占用的 Slot 的 `Version` 原子自增。
        
    - 如果旧模块持有过期 ID 去路由，虽然 `Index` 命中了物理槽位，但通过比对槽位当前 `Version` 与 ID 中的 `Version`，即可 100% 识别出“查无此人”。
        

### C. 内存所有权：Non-owning 裸指针 vs 智能指针

- **决策**：Registry 内部仅存储 `std::atomic<Actor*>` 裸指针，不使用 `std::shared_ptr` 或 `std::unique_ptr`。
    
- **理由**：Registry 仅作“电话簿”，**不拥有** Actor 的生命周期。Actor 的生杀大权归属于对象池与调度器。使用裸指针避免了原子引用计数的性能灾难，完美契合 `std::atomic` 的并发原语要求。
    

## 3. 核心机制深度拆解 (Implementation Deep Dive)

### A. 物理与逻辑内存对齐的 ID 结构

```
union ActorID {
    struct {
        uint32_t index;   // 路由槽位 (O(1) 寻址)
        uint32_t version; // 身份防伪码 (防 ABA)
    } parts;
    uint64_t raw;         // 64位寄存器级比对、网络序列化、哈希映射
};
```

利用 `union` 消除比较结构体时的多条汇编指令，在 64 位机器上 `id1.raw == id2.raw` 仅需单条单周期 CPU 指令。

### B. O(1) 无锁查询 (The Hot Path: Get)

这是全系统调用最频密的接口，通过极其严苛的指令编排防范 UAF (Use-After-Free) 漏洞。

```
Actor* get(ActorID id) {
    if (id.parts.index >= MAX_ACTORS) [[unlikely]] return nullptr;

    // 1. Acquire 语义加载指针，确保可见性
    Actor *ptr = actors_[idx].load(std::memory_order_acquire);
    if (!ptr) [[unlikely]] return nullptr;

    // 2. 核心安全屏障：绝对禁止执行 ptr->id()！
    // 直接读取外部版本的 versions_ 数组。因为此时 ptr 可能已被物理 delete。
    uint32_t current_ver = versions_[idx].load(std::memory_order_relaxed);
    if (current_ver != id.parts.version) [[unlikely]] return nullptr; 

    return ptr;
}
```

### C. O(1) Wait-free 逻辑摘除 (The Remove Flow)

```
void remove(ActorID id) {
    // ... 版本号校验 ...
    Actor *expected = actors_[idx].load(std::memory_order_acquire);
    
    // 无 while 循环的强 CAS！保证 Wait-free
    if (actors_[idx].compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed)) {
        versions_[idx].fetch_add(1, std::memory_order_relaxed);
        free_indices_.enqueue(idx);
    }
}
```

- **为什么是 Wait-free？** 这里**没有失败重试循环**。如果 CAS 失败，意味着该 Actor 已经被并发的其他线程摘除。基于状态机幂等性，当前线程的目标已达成，直接返回。保证了严格的有限步执行。
    

## 4. 并发安全性与内存序考量 (Concurrency & Memory Order)

严格遵循 **Acquire-Release 语义** 构建 Happens-Before 关系：

1. **安全发布 (Safe Publication)**：`create_actor` 在分配完内存并调用构造函数后，使用 `actors_[idx].store(actor, std::memory_order_release)`。确保对象内部数据初始化先于指针的发布写入主存。
    
2. **安全读取**：`get` 中使用 `actors_[idx].load(std::memory_order_acquire)`。与 `create_actor` 的 Release 形成配对，确保一旦读到非空指针，该 Actor 的所有内部状态对当前读线程绝对可见，杜绝虚表或成员未初始化导致的 Crash。
    
3. **最小开销的 Relaxed**：在版本号校验、版本号自增时，由于其逻辑顺序已被先前的 Queue 占位或 CAS 操作的同步屏障所保证，果断降级为 `std::memory_order_relaxed`，榨干 CPU 性能。
    

## 5. 高级面试推演 (Interview Q&A)

**Q1: 在无锁并发下，为什么不能在发消息前直接 `if (ptr != nullptr)` 检查物理指针？**

**A1:** 会引发致命的 TOCTOU (Time-of-Check to Time-of-Use) 和 ABA 污染。检查通过的下一纳秒，线程可能被挂起，目标对象被内存池回收并重新分配给另一个类型的 Actor。恢复执行时将直接污染新对象的内存。Registry 的“Index 寻址 + Version 校验”正是为了斩断这种物理绑定的静默污染。

**Q2: 你的 `remove` 函数使用了 CAS 操作，它会因为高并发产生自旋等待 (Spinning) 吗？**

**A2:** 不会。一般的 CAS 包含 `while(!CAS)` 是 Lock-free 的。但我的 `remove` 语义是“确保目标不在 Registry 中”。如果 CAS 失败，说明遭遇了极小概率的并发摘除，此时对象实质上已被摘除，我的语义已经满足，直接成功 `return`。代码中没有任何重试循环，指令步数严格有界，因此属于更高级别的 **Wait-free** 设计。

**Q3: 你的 `get` 返回了裸指针，如果外部刚拿到这个指针，还没来得及调用，Actor 就被其他线程 `remove` 并销毁了，还是会崩溃吧？**

**A3:** Registry 的边界仅限于“安全的路由映射 (Mapping)”。要彻底解决执行期的生命周期安全，必须配合上层架构。在我们的引擎中，采用了 **对象池 + 调度器延迟回收 (Epoch-based Reclamation 类似思想)**。Registry `remove` 只是逻辑摘除路由，Actor 的真正物理释放会被调度器推迟到当前所有的安全点（或当前 Dispatch 循环）结束之后。