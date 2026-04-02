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

    // 前置声明 Task
    template <typename T = void>
    struct Task;

    // ==========================================
    // 1. PromiseBase (提取公共逻辑)
    // ==========================================
    struct PromiseBase
    {
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_; // 等待者

        // 1. Initial Suspend: 总是挂起，等待 co_await
        std::suspend_always initial_suspend() noexcept { return {}; }

        // 2. Unhandled Exception: 统一捕获
        void unhandled_exception() { exception_ = std::current_exception(); }

        // 3. Final Awaiter: 统一处理对称转移
        struct FinalAwaiter
        {
            bool await_ready() const noexcept { return false; }

            // 关键点：使用模板适配 Promise<T> 和 Promise<void>
            // 当协程结束时，h 是当前协程的 handle
            template <typename PromiseType>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<PromiseType> h) noexcept
            {
                // 通过 h.promise() 访问继承自 PromiseBase 的 continuation_
                auto &promise = h.promise();

                // 如果有等待者，跳转执行等待者；否则执行 noop (销毁或结束)
                return promise.continuation_ ? promise.continuation_ : std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }
    };

    // ==========================================
    // 2. Promise<T> (继承 Base)
    // ==========================================
    template <typename T>
    struct Promise : public PromiseBase
    {
        T value_;

        Task<T> get_return_object() noexcept;

        // 特有逻辑：处理返回值
        template <typename U>
        void return_value(U &&v)
        {
            value_ = std::forward<U>(v);
        }
    };

    // ==========================================
    // 3. Promise<void> (继承 Base)
    // ==========================================
    template <>
    struct Promise<void> : public PromiseBase
    {
        Task<void> get_return_object() noexcept;

        // 特有逻辑：void 返回
        void return_void() {}
    };

    // ==========================================
    // 4. Task<T> 实现
    // ==========================================
    template <typename T>
    struct [[nodiscard("Task must be co_awaited")]] Task
    {
        using promise_type = Promise<T>;
        using handle_type = std::coroutine_handle<promise_type>;

        handle_type handle_;

        explicit Task(handle_type h) noexcept : handle_(h) {}

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

        // 禁用拷贝
        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;

        // --- Awaiter 接口 ---
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
        {
            // 访问 Base 中的 continuation_
            handle_.promise().continuation_ = caller;
            return handle_;
        }

        T await_resume()
        {
            // 访问 Base 中的 exception_
            if (handle_.promise().exception_)
                std::rethrow_exception(handle_.promise().exception_);

            if constexpr (!std::is_void_v<T>)
            {
                return std::move(handle_.promise().value_);
            }
        }
    };

    // ==========================================
    // 5. 延迟实现的 get_return_object
    // ==========================================
    // 必须在 Task 定义完整后实现，因为 Promise 需要构造 Task
    template <typename T>
    Task<T> Promise<T>::get_return_object() noexcept
    {
        return Task<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
    }

    inline Task<void> Promise<void>::get_return_object() noexcept
    {
        return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
    }

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

    class MoveOnlyTask
    {
        struct Base
        {
            virtual ~Base() = default;
            virtual void call() = 0;
        };

        template <typename F>
        struct Derived : Base
        {
            F f;
            Derived(F &&func) : f(std::forward<F>(func)) {}
            void call() override { f(); }
        };

        std::unique_ptr<Base> ptr_;

    public:
        MoveOnlyTask() = default;

        template <typename F>
        MoveOnlyTask(F &&f) : ptr_(std::make_unique<Derived<std::decay_t<F>>>(std::forward<F>(f))) {}

        void operator()()
        {
            if (ptr_)
                ptr_->call();
        }

        // 禁止拷贝
        MoveOnlyTask(const MoveOnlyTask &) = delete;
        MoveOnlyTask &operator=(const MoveOnlyTask &) = delete;

        // 允许移动
        MoveOnlyTask(MoveOnlyTask &&) noexcept = default;
        MoveOnlyTask &operator=(MoveOnlyTask &&) noexcept = default;

        explicit operator bool() const { return !!ptr_; }
    };

} // namespace aegis::core