#pragma once
#include <coroutine>
#include <exception>

namespace aegis::core
{

    struct Task
    {
        struct promise_type
        {
            Task get_return_object() { return {}; }
            std::suspend_never initial_suspend() { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() {}
            void unhandled_exception() { std::terminate(); }
        };
    };

} // namespace aegis::core