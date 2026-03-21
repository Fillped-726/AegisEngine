# Aegis Engine: Core Runtime Module (核心运行时)

### 1. 模块全景图 (The Big Picture)

**目标：** 构建一个基于 **M:N 线程模型** 的高并发、低延迟驱动引擎，将数万个逻辑流（协程/Actor）映射到少量的物理线程上执行。

- **定位：** **Engine Heart (引擎心脏)**。它是系统的动力源，位于 `Common` 基础库之上，支撑起上层的 `Actor` 逻辑层。
    
- **职责边界：**
    
    - **负责：** CPU 算力调度（Work Stealing）、IO 事件驱动（io_uring Proactor）、协程状态机流转（Suspend/Resume）、高精度定时器管理。
        
    - **不负责：** 具体业务逻辑实现、网络协议解析（由 Net 层负责）、数据持久化。
        
- **依赖关系：**
    
    - **下层：** 深度依赖 `common::WorkStealingQueue` (调度), `common::ObjectPool` (协程帧分配), `net::Connection` (IO 对象)。
        
    - **上层：** 直接驱动 `Actor System`，为业务逻辑提供 `co_await` 原语支持。
        

### 2. 架构与类图 (Architecture & Topology)

**目标：** 展示“计算”、“IO”、“时间”三驾马车如何协同工作。

#### 核心架构拓扑

整个运行时由三个单例组件构成三角支撑：

1. **Scheduler (CPU):** 管理一组 Worker 线程，负责执行就绪的 Task。
    
2. **Env (IO):** 独占一个 IO 线程，负责向内核提交/收割 IO 请求。
    
3. **HierarchyTimer (Time):** 负责时间维度的事件触发。
    

#### 核心类关系图

```
classDiagram
    %% 核心单例
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

    %% 协程原语
    class Task~T~ {
        +promise_type
        +handle
    }
    class Actor {
        +process_batch()
        +push_message()
    }

    %% 关系
    Scheduler "1" *-- "N" WorkerThread : Manages
    WorkerThread ..> Actor : 1. Executes
    Actor ..> Task : 2. Returns
    Task ..> Env : 3. Awaits IO
    Task ..> HierarchyTimer : 3. Awaits Time
    Env ..> Scheduler : 4. Dispatches Ready Actor
    HierarchyTimer ..> Scheduler : 4. Dispatches Ready Actor
```

**数据流向 (The Loop):**

1. **Worker** 从队列取出 `Actor` 执行。
    
2. **Actor** 代码遇到 `co_await socket.read()`，挂起。
    
3. **Task** 将请求注册到 `Env`，控制权交还 Worker。
    
4. **Worker** 继续执行下一个 `Actor`（无阻塞）。
    
5. **Env** 收到内核 IO 完成事件，将 `Actor` 重新推入 `Scheduler` 就绪队列。
    

### 3. 核心交互流程 (Key Workflows)

**目标：** 展示系统如何处理异步操作而不阻塞物理线程。

#### 流程 A：协程的挂起与 IO 唤醒 (The Async IO Cycle)

这是 Proactor 模式在 Aegis 中的典型体现。

```
sequenceDiagram
    participant Worker as Worker Thread
    participant Actor as User Logic
    participant Env as IO Thread
    participant Kernel as io_uring
    participant Sched as Scheduler

    Note over Worker, Actor: 1. 执行业务逻辑
    Worker->>Actor: actor->process()
    Actor->>Actor: co_await socket.recv()
    
    Note over Actor, Env: 2. 挂起并提交 IO
    Actor->>Env: prepare_read_sqe()
    Actor-->>Worker: suspend (return control)
    Worker->>Worker: steal_next_task()

    Note over Env, Kernel: 3. 异步处理
    Env->>Kernel: io_uring_submit()
    Kernel-->>Env: CQE Ready (Data Arrived)

    Note over Env, Sched: 4. 唤醒流程
    Env->>Actor: set_result(bytes)
    Env->>Sched: dispatch(Actor)
    Sched->>Worker: enqueue(Actor)
```

#### 流程 B：工作窃取与负载均衡 (Work Stealing)

解决“一核有难，八核围观”的 CPU 负载不均问题。

1. **Local Pop:** Worker 优先访问自己的 `LocalQueue` (Lock-Free, LIFO)，命中率高，Cache 友好。
    
2. **Empty:** 本地队列为空。
    
3. **Steal:** 随机选择一个受害者 (Victim Worker)。
    
4. **CAS Loop:** 尝试从受害者的 `GlobalQueue` (FIFO) 头部偷取一半任务。
    
5. **Execution:** 偷窃成功，立即执行；失败则指数退避 (Spin -> Sleep)。
    

### 4. 关键技术决策与取舍 (Key Design Decisions)

**目标：** 深度解析高性能背后的设计哲学。

#### A. 调度模型：M:N 协程调度 vs 1:1 线程调度

- **决策：** 使用 C++20 无栈协程实现 M:N 模型（M 个协程运行在 N 个线程上）。
    
- **收益：**
    
    - **内存节省：** 协程栈仅需几百字节（vs 线程栈 1MB+）。
        
    - **切换极速：** 用户态切换仅需 ~10ns（vs 内核态线程切换 ~1-2us）。
        
- **代价：** 必须严格杜绝阻塞操作（如 `std::sleep` 或阻塞 IO），否则会卡死整个物理线程。
    

#### B. IO 模型：Proactor (io_uring) + SQPOLL

- **决策：** 采用 Proactor 模式，由独立的 IO 线程集中提交请求。开启 `IORING_SETUP_SQPOLL`。
    
- **方案对比：**
    
    - _vs Reactor (epoll):_ Reactor 仅通知“可读”，用户仍需调用 `read` 系统调用。Proactor 通知“读完了”，省去了数据拷贝的系统调用。
        
    - _vs Thread-per-Core IO:_ 全局单 IO 线程降低了跨线程连接管理的复杂度，且利用 SQPOLL 实现了 **Zero-Syscall** 的请求提交。
        

#### C. 协程原语：Symmetric Transfer (对称转移)

- **决策：** 在 `Task::await_suspend` 中使用对称转移技术。
    
- **收益：** 解决了协程递归调用导致的 **Stack Overflow (栈溢出)** 问题。
    
- **原理：** 利用 C++20 的 Tail Call Optimization，当前协程挂起后，直接跳转到下一个协程的句柄，而不是递归调用 `resume()`。这使得异步调用链的深度可以无限长。
    

#### D. 时间管理：分层时间轮 (Hierarchical Timing Wheel)

- **决策：** 模仿 Linux 内核，使用 5 层时间轮管理定时器。
    
- **收益：** 将定时器的插入、删除、执行复杂度从 O(logN) (红黑树/堆) 降低到 **O(1)**。
    
- **优化：** 引入 `pending_adds` 双缓冲队列，将多线程的锁竞争限制在极小的临界区内，实际插入操作由 IO 线程无锁完成。
    

### 5. 对外接口与用法 (API & Usage)

**目标：** 展示如何驱动这个引擎。

```
// 1. 启动运行时环境
int main() {
    // 初始化 IO 环境 (RingDepth = 4096)
    aegis::core::Env::instance().init(4096);
    
    // 初始化时间轮
    aegis::core::HierarchicalTimeWheel::instance().init();
    
    // 启动 IO 线程
    std::thread io_thread([]{ aegis::core::Env::instance().run(); });

    // 启动 CPU 调度器 (4 Workers)
    aegis::core::Scheduler::instance().start(4);

    // 2. 投递入口任务 (DetachedTask)
    // 实际业务中，这通常是 Acceptor 或者是一个 Actor 的启动
    auto startup_task = []() -> aegis::core::DetachedTask {
        // 模拟业务
        LOG_INFO("Engine started!");
        co_await aegis::core::sleep(1000); // 异步休眠 1s
        LOG_INFO("1 second later...");
    };
    startup_task();

    // 主线程阻塞等待
    while(true) std::this_thread::sleep_for(std::chrono::seconds(1));
}
```