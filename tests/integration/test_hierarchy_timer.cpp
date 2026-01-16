#include "aegis/core/hierarchy_timer.h"
#include <iostream>
#include <thread>

using namespace aegis::core;

int main()
{
    // 初始化日志
    aegis::Log::instance().set_level(spdlog::level::debug);

    // [Fix] 获取单例引用，而不是创建栈对象
    auto &tw = HierarchicalTimeWheel::instance();

    // 初始化 timerfd (Phase 3 必需，Phase 2 测试可选，但建议加上)
    tw.init();

    std::cout << "--- Phase 2: Thread-Safety & Cancel Verification ---" << std::endl;

    // 1. 测试添加
    tw.add_timer(5, []()
                 { std::cout << "Task A (5 tick) executed!" << std::endl; });

    // 2. 测试取消
    tw.add_timer(10, []()
                 { std::cout << "Task B (10 tick) executed! (ERROR: Should be canceled)" << std::endl; });
    tw.cancel_timer(102);

    std::cout << ">> Timers added to pending queue. Starting Tick Loop..." << std::endl;

    // 模拟 Tick 循环
    for (int i = 0; i < 15; ++i)
    {
        if (i == 5)
            std::cout << ">> Tick 5 (Expect Task A)" << std::endl;
        if (i == 10)
            std::cout << ">> Tick 10 (Expect No Task B)" << std::endl;

        tw.tick();
    }

    std::cout << "--- Test Finished ---" << std::endl;
    return 0;
}