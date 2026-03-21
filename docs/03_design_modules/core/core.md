# Aegis Engine: Core Runtime Module Technical Documentation

> **Status:** Engine Heart / Execution Driver
> 
> **Language:** C++20 (Coroutines)
> 
> **Key Attributes:** M:N Scheduling, Proactor, Work Stealing, Zero-Syscall IO

## 1. 模块全景图 (The Big Picture)

### 1.1 定位与目标

**Core Runtime** 是 Aegis 引擎的 **动力源 (Engine Heart)**。

- **目标：** 构建基于 **M:N 线程模型** 的高并发驱动，将数万个逻辑流（协程/Actor）高效映射到少量物理线程。
    
- **职责：** 负责 CPU 算力调度、IO 事件驱动、协程状态机流转、高精度时间管理。
    

### 1.2 职责边界

|   |   |
|---|---|
|**负责 (In-Scope)**|**不负责 (Out-of-Scope)**|
|**CPU 调度** (Work Stealing, Load Balance)|具体业务逻辑实现|
|**IO 驱动** (io_uring Proactor, SQPOLL)|网络协议解析 (Net 层)|
|**协程流转** (Suspend/Resume, Symmetric Transfer)|数据持久化|
|**时间管理** (Hierarchy Timing Wheel)||

### 1.3 依赖关系

- **下层依赖:** `common::WorkStealingQueue` (调度容器), `common::ObjectPool` (协程帧内存), `net::Connection` (IO 对象)。
    
- **上层支撑:** `Actor System` (提供 `co_await` 原语与执行环境).
    

## 2. 架构与类图 (Architecture)

运行时由 **计算 (Scheduler)**、**IO (Env)**、**时间 (Timer)** 三驾马车构成三角支撑。

### 2.1 核心组件

1. **Scheduler (CPU):** 管理 Worker 线程池，执行就绪 Task。核心是 **Work Stealing** 算法。
    
2. **Env (IO):** 独占 IO 线程，作为 Proactor 核心，负责向内核提交/收割 `io_uring` 请求。
    
3. **HierarchyTimer (Time):** 5层时间轮，管理所有延时与周期任务，提供 O(1) 复杂度。
    

### 2.2 类关系图 (Class Diagram)

```
classDiagram
    %% Core Singletons
    class Scheduler {
        +workers: vector~Thread~
        +queues: vector~WSQ~
        +dispatch(Actor*)
        -steal()
    }
    class Env {
        +ring: io_uring
        +run()
        +submit(sqe)
    }
    class HierarchyTimer {
        +add_timer()
        +tick()
    }

    %% Coroutine Primitives
    class Task~T~ {
        +promise_type
        +handle
        +await_suspend()
    }
    class Actor {
        +process_batch()
        +push_message()
    }

    %% Relationships
    Scheduler "1" *-- "N" WorkerThread : Manages
    WorkerThread ..> Actor : 1. Executes
    Actor ..> Task : 2. Returns (Coroutine)
    Task ..> Env : 3. Awaits IO
    Task ..> HierarchyTimer : 3. Awaits Time
    Env ..> Scheduler : 4. Dispatches Ready Actor
    HierarchyTimer ..> Scheduler : 4. Dispatches Ready Actor
```

### 2.3 数据流向 (The Loop)

1. **Worker** 从队列取出 `Actor` 执行。
    
2. **Actor** 遇 `co_await socket.read()` 挂起。
    
3. **Task** 将请求注册到 `Env`，利用 **Symmetric Transfer** 将控制权交还 Worker。
    
4. **Worker** 继续执行下一个 `Actor` (无阻塞)。
    
5. **Env** 收到内核 IO 完成事件 (CQE)，将 `Actor` 重新推入 `Scheduler` 就绪队列。
    

## 3. 核心交互流程 (Key Workflows)

### 3.1 协程挂起与 IO 唤醒 (The Async IO Cycle)

_亮点：Proactor 模式的典型体现，业务层无感知的异步化。_

```
sequenceDiagram
    participant Worker as Worker Thread
    participant Actor as User Logic
    participant Env as IO Thread
    participant Kernel as io_uring (SQPOLL)
    participant Sched as Scheduler

    Note over Worker, Actor: 1. 执行业务逻辑
    Worker->>Actor: actor->process()
    Actor->>Actor: co_await socket.recv()
    
    Note over Actor, Env: 2. 挂起并提交 IO
    Actor->>Env: prepare_read_sqe()
    Actor-->>Worker: suspend (Symmetric Transfer)
    Worker->>Worker: steal_next_task()

    Note over Env, Kernel: 3. 异步处理 (Zero-Syscall)
    Env->>Kernel: io_uring_submit() (Kernel Thread polls SQ)
    Kernel-->>Env: CQE Ready (Data Arrived)

    Note over Env, Sched: 4. 唤醒流程
    Env->>Actor: set_result(bytes)
    Env->>Sched: dispatch(Actor)
    Sched->>Worker: enqueue(Actor)
```

### 3.2 工作窃取与负载均衡 (Work Stealing)

_场景：解决“一核有难，八核围观”的 CPU 负载不均。_

1. **Local Pop:** Worker 优先访问自己的 `LocalQueue` (Lock-Free, LIFO)。**Cache Hit 高**。
    
2. **Empty:** 本地队列为空。
    
3. **Steal:** 随机选择一个受害者 (Victim Worker)。
    
4. **CAS Loop:** 尝试从受害者的 `GlobalQueue` (FIFO) 头部偷取一半任务。
    
5. **Execution:** 偷窃成功立即执行；失败则指数退避 (Spin -> Sleep)。
    

## 4. 关键技术决策 (Key Design Decisions)

|   |   |   |   |
|---|---|---|---|
|**技术点**|**决策理由 (Why)**|**收益 (Benefit)**|**代价/难点**|
|**M:N 协程调度**|C++20 无栈协程 (Stackless) 实现。|**内存节省** (几百字节/协程 vs 1MB/线程)；**切换极速** (~10ns vs ~2us)。|严禁阻塞操作，否则卡死物理线程。|
|**Proactor + SQPOLL**|独立的 IO 线程 + 内核轮询。|**Zero-Syscall** 提交请求；Proactor 省去数据拷贝系统调用。|需独占一个 CPU 核心给内核轮询线程。|
|**Symmetric Transfer**|`await_suspend` 返回下一协程句柄。|利用 **Tail Call Optimization (TCO)**，防止深层异步调用的 **Stack Overflow**。|编译器优化依赖，需精细控制协程句柄生命周期。|
|**分层时间轮**|模仿 Linux 内核 5 层时间轮。|定时器操作 (Add/Del/Run) 复杂度从 O(logN) 降至 **O(1)**。|实现复杂；需处理 Tick 精度与 CPU 唤醒频率的平衡。|

## 5. 接口示例 (API Usage)

```
// 1. 启动运行时环境
int main() {
    // A. 初始化 IO 环境 (RingDepth = 4096, 开启 SQPOLL)
    aegis::core::Env::instance().init(4096);
    
    // B. 初始化时间轮
    aegis::core::HierarchicalTimeWheel::instance().init();
    
    // C. 启动 IO 线程 (Proactor Loop)
    std::thread io_thread([]{ aegis::core::Env::instance().run(); });

    // D. 启动 CPU 调度器 (4 Workers, M:N Mapping)
    aegis::core::Scheduler::instance().start(4);

    // 2. 投递入口任务 (DetachedTask)
    auto startup_task = []() -> aegis::core::DetachedTask {
        LOG_INFO("Engine started!");
        
        // 异步休眠 1s (不阻塞线程，只挂起协程)
        co_await aegis::core::sleep(1000); 
        
        LOG_INFO("1 second later...");
    };
    startup_task();

    // 主线程阻塞保活
    while(true) std::this_thread::sleep_for(std::chrono::seconds(1));
}
```

## 6. 面试转化与简历亮点 (Resume Highlights)

**在简历描述 Core Runtime 模块时，建议强调以下关键词：**

- **M:N 高并发线程模型：** 设计并实现了基于 **C++20 Stackless Coroutines** 的 M:N 调度器，将数万个逻辑协程映射到少量物理线程执行。通过 **Work Stealing (工作窃取)** 算法实现了多核 CPU 的负载均衡，有效解决了尾延迟问题。
    
- **零系统调用 IO (Zero-Syscall IO)：** 采用 **Proactor** 模式封装 `io_uring`，并启用了 **SQPOLL (Submission Queue Polling)** 特性，使得用户态提交 IO 请求完全无需陷入内核 (No Syscall)，将 IO 提交开销降至纳秒级。
    
- **对称控制流转移 (Symmetric Transfer)：** 深入利用 C++20 协程特性，在 `await_suspend` 中实现了 **Symmetric Transfer** 机制，利用编译器的 **Tail Call Optimization (TCO)** 彻底解决了异步递归调用导致的栈溢出风险，支持无限深度的异步调用链。
    
- **O(1) 高精度定时器：** 参考 Linux 内核实现，设计了 **多级分层时间轮 (Hierarchical Timing Wheel)**，将定时器的插入与触发复杂度从红黑树的 O(logN) 降低至 **O(1)**，并引入 **双缓冲队列 (Double-Buffering)** 最小化了多线程插入时的锁竞争。