#pragma once
#include <coroutine>

namespace aegis::core
{
    // 所有异步操作的基类
    struct BaseAwaiter
    {
        int result = 0;
        std::coroutine_handle<> handle;

        ~BaseAwaiter() = default;
    };

} // namespace aegis::core