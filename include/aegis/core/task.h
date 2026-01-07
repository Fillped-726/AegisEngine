#pragma once
#include <coroutine>
#include <exception>
#include <utility> // for std::move, std::exchange
#include <cassert>

namespace aegis::core
{

    template <typename T>
    struct Promise;

    // --- Task 定义 ---
    template <typename T = void>
    struct Task
    {
        using promise_type = Promise<T>;
        using handle_type = std::coroutine_handle<promise_type>;

        handle_type handle_;

        Task(handle_type h) : handle_(h) {}

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

        // --- Awaitable 接口 ---
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
        {
            handle_.promise().continuation_ = caller;
            return handle_; // 立即启动 Lazy 的协程
        }

        // 核心修复：使用 if constexpr 处理 void 情况
        T await_resume()
        {
            if (handle_.promise().exception_)
                std::rethrow_exception(handle_.promise().exception_);

            // 只有当 T 不是 void 时，才去访问 value_
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
        std::coroutine_handle<> continuation_;

        Task<T> get_return_object()
        {
            return Task<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
        }

        // 必须是 always，因为 Task 是被等待的
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter
        {
            bool await_ready() const noexcept { return false; }
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
        // 这里的 value_ 已经移除了，这很正确
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_;

        Task<void> get_return_object()
        {
            return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
        }

        // co_wait 时必须挂起,协程必须声明为懒汉式
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

    struct DetachedTask
    {
        struct promise_type
        {
            // 1. 立即执行 (Eager Execution)
            std::suspend_never initial_suspend() noexcept { return {}; }

            // 2. 结束后自动销毁 (Fire and Forget 的核心)
            // 注意：这里返回 suspend_never，表示协程体执行完后，不挂起，直接销毁协程帧
            std::suspend_never final_suspend() noexcept { return {}; }

            void return_void() {}

            DetachedTask get_return_object() { return {}; }

            // 3. 异常处理：因为没人等待我们，所以必须在这里捕获
            void unhandled_exception()
            {
                try
                {
                    std::rethrow_exception(std::current_exception());
                }
                catch (const std::exception &e)
                {
                    // 实际项目中建议打印日志，而不是 stderr
                    fprintf(stderr, "[DetachedTask] Unhandled exception: %s\n", e.what());
                }
            }
        };
    };

} // namespace aegis::core