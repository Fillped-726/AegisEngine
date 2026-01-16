#pragma once
#include <coroutine>

namespace aegis::core
{
    // 所有异步操作的基类
    // 必须有虚析构函数，因为在某些高级用法中可能会通过基类指针删除
    struct BaseAwaiter
    {
        int result = 0;
        std::coroutine_handle<> handle;

        ~BaseAwaiter() = default;
    };

} // namespace aegis::core