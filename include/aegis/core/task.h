#pragma once

#include <coroutine>
#include <exception>
#include <utility> // for std::move, std::exchange
#include <cassert>
#include <type_traits>

#include "aegis/common/aegisLog.h" // 统一日志出口

namespace aegis::core
{

    template <typename T>
    struct Promise;

    /**
     * @brief 标准异步任务 (Lazy)
     * @details
     * 1. Lazy Execution: 创建时挂起，直到被 co_await 时才执行。
     * 2. Symmetric Transfer: 利用 await_suspend 返回 handle 进行尾调用优化。
     * 3. Single Ownership: 只能被 move，不能 copy。
     */
    template <typename T = void>
    struct Task
    {
        using promise_type = Promise<T>;
        using handle_type = std::coroutine_handle<promise_type>;

        handle_type handle_;

        explicit Task(handle_type h) : handle_(h) {}

        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;

        Task(Task &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

        Task &operator=(Task &&other) noexcept
        {
            if (this != &other)
            {
                if (handle_)
                    handle_.destroy();
                handle_ = std::exchange(other.handle_, nullptr);
            }
            return *this;
        }

        ~Task()
        {
            if (handle_)
                handle_.destroy();
        }

        // --- Awaitable Interface ---

        // 返回 false 表示"不立即就绪"，强制调用 await_suspend
        bool await_ready() const noexcept { return false; }

        // 对称转移核心：
        // 1. 保存当前调用者 (caller) 到 promise 中。
        // 2. 返回自己的 handle，告诉编译器"暂停 caller，立即跳转执行我"。
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
        {
            handle_.promise().continuation_ = caller;
            return handle_;
        }

        T await_resume()
        {
            // 异常传播：如果在协程中抛出了异常，在这里重新抛出给等待者
            if (handle_.promise().exception_)
                std::rethrow_exception(handle_.promise().exception_);

            // 使用 if constexpr 编译期分支，处理 void 和非 void 的统一接口
            if constexpr (!std::is_void_v<T>)
            {
                return std::move(handle_.promise().value_);
            }
        }
    };

    // --- Promise<T> (通用版) ---
    template <typename T>
    struct Promise
    {
        T value_;
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_; // 等待我的协程

        Task<T> get_return_object()
        {
            return Task<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
        }

        // Initial Suspend: Always
        // 确保任务创建后不立即跑，而是等待 co_await 或手动 resume
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter
        {
            bool await_ready() const noexcept { return false; }

            // 协程结束时的对称转移：
            // 如果有 continuation (等待者)，跳转过去；否则(根协程)执行 noop。
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise<T>> h) noexcept
            {
                auto continuation = h.promise().continuation_;
                return continuation ? continuation : std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T &&v) { value_ = std::move(v); }
        void return_value(const T &v) { value_ = v; }
        void unhandled_exception() { exception_ = std::current_exception(); }
    };

    // --- Promise<void> (特化版) ---
    template <>
    struct Promise<void>
    {
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_;

        Task<void> get_return_object()
        {
            return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter
        {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise<void>> h) noexcept
            {
                auto continuation = h.promise().continuation_;
                return continuation ? continuation : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() {}
        void unhandled_exception() { exception_ = std::current_exception(); }
    };

    /**
     * @brief "Fire-and-Forget" 任务
     * @details
     * 用于 Actor 的入口函数。它立即执行 (Eager)，并且自己管理生命周期。
     * 遇到异常时，因为没人 await 它，只能打日志兜底。
     */
    struct DetachedTask
    {
        struct promise_type
        {
            // 1. Eager Execution: 创建即运行，无需 co_await
            std::suspend_never initial_suspend() noexcept { return {}; }

            // 2. Auto Destroy: 执行完立即销毁协程帧
            std::suspend_never final_suspend() noexcept { return {}; }

            void return_void() {}

            DetachedTask get_return_object() { return {}; }

            // 3. 异常兜底
            void unhandled_exception()
            {
                try
                {
                    std::rethrow_exception(std::current_exception());
                }
                catch (const std::exception &e)
                {
                    // [Fix] 路由到 Aegis Log 系统
                    aegis::Log::instance().error("[DetachedTask] Uncaught exception in actor/task: {}", e.what());
                }
                catch (...)
                {
                    aegis::Log::instance().error("[DetachedTask] Unknown exception caught.");
                }
            }
        };
    };

} // namespace aegis::core