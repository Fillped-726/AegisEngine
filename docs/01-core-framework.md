# 核心框架 — Actor / 协程 / 调度器 / 定时器

> 对应目录: `include/aegis/core/` · `src/core/`

---

## 1. Actor 模型

### 1.1 Actor 基类 (`actor.h`)

```cpp
class Actor {
    // ── 身份 ──
    ActorID id_;             // 64-bit: index(32) + version(32)
    ActorID parent_id_;      // 监管树父节点
    int worker_id_;          // 绑定的 Worker (-1 = 未分配)

    // ── 无锁消息队列 (MPSC) ──
    ActorMessage *head_;                               // Consumer: 仅绑定 Worker 读取
    std::atomic<ActorMessage *> tail_;                 // Producer: 任意线程 push
    std::atomic<bool> is_scheduled_;                   // 调度状态机

    // ── 生命周期状态机 ──
    enum ActorState { Active, Idle, Dead };

    // ── 核心 API ──
    template<typename T> bool push(T *msg);            // 线程安全入队
    ActorState process_batch(int budget = 100);         // 消费消息
    virtual void handle_message(ActorMessage *msg) = 0; // 子类实现
    virtual void finalize() = 0;                        // 子类实现
    TimerId schedule_timer(uint32_t delay_ms, std::function<void()> cb);
};
```

**MPSC 入队逻辑 (`push`)**:
1. `msg->next.store(nullptr, relaxed)`
2. `prev = tail_.exchange(msg, acq_rel)` — 原子交换 Tail
3. `prev->next.store(msg, release)` — 链接旧尾到新节点
4. 若 `is_scheduled_` 从 false→true，返回 true 触发调度

**缓存行对齐**: Actor 的关键字段按 64 字节缓存行对齐，防止 false sharing。

### 1.2 生命周期策略 (`actor_traits.h`)

| CRTP 包装 | 分配方式 | 适用场景 | 示例 |
|-----------|---------|---------|------|
| `SimpleActor<T>` | `new` / `delete` | 管理类（数量少、生命周期长） | RoomManager |
| `PooledActor<T,N,M>` | `ObjectPool<T,N,M>` | 高频创建/销毁（数量大、池复用） | PlayerActor / SceneActor / NpcActor |

```cpp
// SimpleActor: 直接 new/delete
class RoomManager : public SimpleActor<RoomManager> { ... };

// PooledActor: 池分配，N=池容量, M=批大小
class PlayerActor : public PooledActor<PlayerActor> { ... };       // 默认 <100000,128>
class SceneActor : public PooledActor<SceneActor, 128, 32> { ... };
class NpcActor   : public PooledActor<NpcActor, 256, 64> { ... };
```

### 1.3 ActorID & ActorRegistry (`actor_registry.h`)

```cpp
union ActorID {
    struct { uint32_t index; uint32_t version; };  // 32+32
    uint64_t raw;  // 可整体比较/哈希/传输
};
```

- `index`: Registry 数组下标 (0~65534)
- `version`: 每次复用 +1，防止 **ABA 问题**
- `raw == 0` 表示无效 ID

**Registry 工作流**:
```
create_actor<T>(args...):
  1. free_indices_.try_dequeue(idx)   ← moodycamel ConcurrentQueue
  2. version = versions_[idx]          ← atomic relaxed
  3. id = {index, version}
  4. actor = T::create(id, args...)    ← 构造（池分配）
  5. actor->set_worker_id(current_worker)
  6. actors_[idx].store(actor, release)
  7. return id

get(ActorID):
  1. actor = actors_[id.index].load(acquire)
  2. actor->id().version != id.version → 版本不匹配 → 返回 nullptr
  3. return actor
```

**内存占用**: `actors_[65536]` = 512KB + `versions_[65536]` = 256KB ≈ 768KB。

---

## 2. 协程系统 (`task.h`)

### 2.1 Task<T>

```cpp
template<typename T>
struct [[nodiscard]] Task {
    using promise_type = Promise<T>;
    handle_type handle_;

    // Eager start: initial_suspend = suspend_always
    // 等待者通过 await_suspend 注册 continuation
    // final_suspend 执行对称转移 → 恢复等待者
};
```

**Promise 层次**:

```
PromiseBase
├── exception_              ← 异常传播
├── continuation_           ← 等待者 coroutine_handle
├── initial_suspend()       ← suspend_always (被 co_await 才开始)
├── final_suspend()         ← FinalAwaiter (对称转移)
└── unhandled_exception()   ← 异常捕获

Promise<T> : PromiseBase    ← 存 value_，return_value()
Promise<void> : PromiseBase ← return_void()
```

### 2.2 DetachedTask

```cpp
struct DetachedTask {
    struct promise_type {
        suspend_never initial_suspend();  // Eager: 创建即执行
        suspend_never final_suspend();    // Auto destroy
        void unhandled_exception();       // 异常兜底 + 日志
        void return_void();
    };
};
```

用于 Actor 的**入口函数**（如 GateServer::handle_session）。不需要被 `co_await`，创建后自动执行。异常不会崩溃，路由到 Log 系统。

### 2.3 MoveOnlyTask

类型擦除的可移动任务包装器。用于 Worker 的自定义任务队列：

```cpp
Worker::post_custom_task(MoveOnlyTask task);  // 线程安全
```

### 2.4 协程链执行路径

```cpp
Task<void> handler(Actor* actor, const LoginReq& req) {
    auto result = co_await db_query(req.user_id);  // 挂起，提交 io_uring SQE
    auto response = build_response(result);         // 恢复后继续
    co_await actor->send_packet(MSG_LOGIN_RSP, response);
}
```

1. `handler` 被**同步调用**（`initial_suspend = suspend_always` → 被 co_await 后才开始）
2. `co_await db_query()` → 提交 io_uring SQE → **挂起当前协程**
3. Worker 的 io_uring 完成循环 → CQE 到达 → `handle.resume()`
4. 恢复执行 → 下一个 `co_await` 或 `co_return`
5. `final_suspend` → FinalAwaiter 对称转移 → 恢复等待者（或 noop）

---

## 3. Scheduler & Worker

### 3.1 Scheduler (`scheduler.h`)

```cpp
class Scheduler {
    void start(int num_workers);       // 启动 N 个 Worker 线程
    void stop();                        // 安全停止
    void dispatch(SchedulerTask task); // Round-Robin 分发 Actor
    Worker* get_worker(int id);         // 获取指定 Worker 指针
};
```

- 单例，仅负责 **Worker 生命周期管理** 和 **初始 Actor 分发**
- `dispatch()` 通过 `rr_counter_` 原子计数器轮询分配
- 每个 Worker 独立拥有 io_uring 实例

### 3.2 Worker (`worker.h`)

```cpp
class Worker {
    void run();                         // 主事件循环
    void stop();
    void post_cross_core_task(SchedulerTask task);   // 跨核通信
    void post_custom_task(MoveOnlyTask task);         // 自定义任务
    void dispatch_local(SchedulerTask task);          // 本地急速派发
    io_uring* ring();
    HierarchicalTimeWheel& time_wheel();
};
```

**Worker 主循环 (run)**:
```
while (running):
  1. process_io(wait, ms_to_next_tick)  ← io_uring CQE 收割
  2. process_local_tasks()              ← Actor mailbox drain
  3. process_cross_core_messages()      ← 其他 Worker 投递的任务
```

**跨核通信**:
```cpp
// Worker A 投递 Actor 到 Worker B
void Worker::post_cross_core_task(SchedulerTask task) {
    cross_core_queue_.enqueue(task);       // moodycamel 无锁队列
    wake_up();                             // eventfd → io_uring
}
```

**唤醒优化**: 使用 `is_waking_up_` 标志位合并高频唤醒，避免 eventfd 风暴。

### 3.3 Thread-Local 上下文

```cpp
extern thread_local int t_worker_id;        // 当前 Worker ID
extern thread_local Worker* t_current_worker; // 当前 Worker 指针
```

---

## 4. 定时器 (`hierarchy_timer.h`)

### 4.1 分层时间轮 (5 级)

```
Level 1 (tv1):  256 slots × 50ms   = 12.8s
Level 2 (tv2):   64 slots × 12.8s  = 13.65min
Level 3 (tv3):   64 slots × 13.65min = 14.56h
Level 4 (tv4):   64 slots × 14.56h  = 38.8 days
Level 5 (tv5):   64 slots × 38.8d   = 6.8 years
```

### 4.2 核心 API

| 操作 | 复杂度 | 说明 |
|------|--------|------|
| `add_timer(delay_ms, cb)` | O(1) | 注册定时器，返回 TimerId |
| `cancel_timer(TimerId)` | O(1) | 取消定时器 |
| `tick()` | O(1) 均摊 | 每 50ms 驱动一次 |

### 4.3 零拷贝级联

使用 `IntrusiveList` 的 splice 操作，定时溢出级联时**移动整个链表**而不是逐个遍历重新插入。

### 4.4 为什么 50ms?

- 对应 20fps 的逻辑更新周期
- 对齐大多数游戏的状态同步帧率
- 不需要纳秒级精确

---

## 5. 序列号生成器 (`sequence_id.h`)

```cpp
class SequenceIDGen {
    uint32_t next();  // 生成全局唯一 SeqID
};
```

**格式**: `[8-bit WorkerID][24-bit Counter]`

- 每个 Worker 独立实例，无跨线程同步
- 24 位 counter 回绕时旧请求已超时
- 用于客户端-服务端请求-响应匹配
