#pragma once
#include <coroutine>

namespace aegis::core
{

    // 所有异步操作的基类
    struct BaseAwaiter
    {
        int result = 0;
        std::coroutine_handle<> handle;

        // 加上虚析构，防止内存泄漏隐患 (虽然目前我们没用 delete base_ptr)
        virtual ~BaseAwaiter() = default;
    };

} // namespace aegis::core