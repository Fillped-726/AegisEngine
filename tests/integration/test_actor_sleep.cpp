// #include <iostream>
// #include <thread>
// #include <chrono>
// #include <sstream> // 必须加上，用于转换 thread::id

// #include "aegis/core/env.h"
// #include "aegis/net/socket.h"
// #include "aegis/common/aegisLog.h"
// #include "aegis/core/task.h"
// // === 宏冲突修复 ===
// #ifdef BLOCK_SIZE
// #undef BLOCK_SIZE
// #endif
// // =================
// #include "aegis/core/sleep.h"
// #include "aegis/core/hierarchy_timer.h"
// #include "aegis/core/actor.h"
// #include "aegis/core/scheduler.h"

// using namespace aegis::core;

// // 辅助函数：安全地将 thread::id 转为 string
// std::string tid_to_string(std::thread::id tid)
// {
//     std::stringstream ss;
//     ss << tid;
//     return ss.str();
// }

// constexpr uint32_t MSG_START_TEST = 100;

// struct StartTestMsg : public ActorMessage
// {
//     StartTestMsg() { type_id = MSG_START_TEST; }
// };

// class TestActor : public Actor
// {
// public:
//     void push_message(std::shared_ptr<ActorMessage> msg)
//     {
//         if (msg->type_id == MSG_TYPE_CORO_WAKEUP)
//         {
//             auto *casted = static_cast<CoroutineWakeupMsg *>(msg.get());
//             // 注意：这里 new 出来的对象，必须确保 Actor 内部处理完后会 delete
//             push(new CoroutineWakeupMsg(casted->handle));
//         }
//         else if (msg->type_id == MSG_START_TEST)
//         {
//             push(new StartTestMsg());
//         }
//     }

//     // 供外部手动调用
//     void push_raw(ActorMessage *msg)
//     {
//         push(msg);
//     }

//     void handle_message(ActorMessage *msg) override
//     {
//         // 只有非系统消息才走这里
//         if (msg->type_id == MSG_START_TEST)
//         {
//             run_async_logic();
//         }
//     }

//     DetachedTask run_async_logic()
//     {
//         auto tid_start = std::this_thread::get_id();
//         aegis::Log::instance().info("[Actor] 🟢 Start Sleep | Thread: {}", tid_to_string(tid_start));

//         auto start_time = std::chrono::steady_clock::now();

//         // 挂起：此时控制权交还给 Scheduler，Worker 线程去处理别的 Actor
//         co_await aegis::core::sleep(1000);

//         // 恢复：此时应该是在某个 Worker 线程中（可能变了，也可能没变）
//         auto end_time = std::chrono::steady_clock::now();
//         // 使用 double 避免精度丢失，虽然 ms 级通常没事
//         auto diff = std::chrono::duration<double, std::milli>(end_time - start_time).count();
//         auto tid_end = std::this_thread::get_id();

//         aegis::Log::instance().info("[Actor] 🔴 Wake Up    | Thread: {} | Elapsed: {:.2f} ms", tid_to_string(tid_end), diff);

//         if (diff < 950 || diff > 1100)
//         {
//             aegis::Log::instance().error("❌ Timing Check Failed! Expected ~1000ms, got {:.2f}ms", diff);
//         }
//         else
//         {
//             aegis::Log::instance().info("✅ Timing Check Passed");
//             aegis::Log::instance().info("✅ Async Flow Finished");
//         }
//     }
// };

// DetachedTask timer_driver()
// {
//     auto &wheel = HierarchicalTimeWheel::instance();
//     wheel.init();
//     int fd = wheel.get_fd();
//     if (fd < 0)
//     {
//         aegis::Log::instance().error("Failed to init timerfd");
//         co_return;
//     }

//     aegis::Log::instance().info("[Timer] Driver running on IO Thread");

//     uint64_t expirations = 0;
//     while (true)
//     {
//         try
//         {
//             // 在 IO 线程挂起等待 timerfd 可读
//             int n = co_await aegis::net::Socket::AsyncRead(fd, &expirations, sizeof(expirations));
//             if (n != 8)
//                 break;

//             // 处理超时任务 -> 触发 TimerNode 回调 -> 发送 MSG_ID_CORO_WAKEUP 给 Actor
//             for (uint64_t i = 0; i < expirations; ++i)
//                 wheel.tick();
//         }
//         catch (...)
//         {
//             break;
//         }
//     }
// }

// int main()
// {
//     aegis::Log::instance().set_level(spdlog::level::debug);
//     aegis::Log::instance().info("[Main] Test Process ID: {}", ::getpid());
//     aegis::Log::instance().info("[Main] IO Thread ID: {}", tid_to_string(std::this_thread::get_id()));

//     aegis::Log::instance().info("[Init] Initializing IO_Uring...");
//     auto &env = Env::instance();
//     env.init();

//     aegis::Log::instance().info("[Init] Starting Scheduler with 2 Workers...");
//     Scheduler::instance().start(2);

//     // 启动 Timer 驱动 (IO 线程)
//     timer_driver();

//     auto actor = new TestActor();

//     // 逻辑优化：先把消息放进去，再 Dispatch，防止多线程下 Actor 被调度两次导致竞争（取决于你的 Scheduler 实现）
//     actor->push_raw(new StartTestMsg());
//     Scheduler::instance().dispatch(actor);

//     aegis::Log::instance().info("[Main] Event Loop Started. Press Ctrl+C to stop.");
//     env.run();

//     return 0;
// }