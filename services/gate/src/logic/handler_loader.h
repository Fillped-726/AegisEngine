#pragma once

#include <memory>

// 前向声明
namespace aegis::core
{
    class SceneActor;
}

namespace aegis::gate
{
    /**
     * @brief 加载所有的业务消息处理器 (Login, Move, etc.)
     * @param global_scene 传递场景指针，供逻辑层操作
     */
    void load_handlers(std::shared_ptr<aegis::core::SceneActor> global_scene);
}