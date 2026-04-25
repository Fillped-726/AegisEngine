/**
 * @file actor_traits.h
 * @brief CRTP wrappers SimpleActor<T> and PooledActor<T> for object lifecycle management.
 */
#pragma once

#include "aegis/core/actor.h"
#include "aegis/common/objectPool.h" // 确保包含你的 ObjectPool 头文件

/**
 * @namespace aegis::core
 * @brief Core engine namespace containing Actor system, scheduling, and game logic.
 */
namespace aegis::core
{
    // ========================================================
    // 策略 1: 普通 Actor (SimpleActor)
    // --------------------------------------------------------
    // 行为：直接使用 new/delete。
    // 适用：RoomManager, GlobalSettings, 单例对象。
    // ========================================================
    template <typename Derived>
    class SimpleActor : public Actor
    {
    public:
        explicit SimpleActor(ActorID self_id)
            : Actor()
        {
            // 立即确立身份
            base_reset(self_id, ActorID(0));
        }

        // [实现契约] 销毁逻辑：直接释放
        void finalize() override
        {
            // 安全检查：确保 Derived 是完整类型
            static_assert(sizeof(Derived) > 0, "Derived must be a complete type");
            delete static_cast<Derived *>(this);
        }

        // [工厂方法] 统一创建入口
        template <typename... Args>
        static Derived *create(Args &&...args)
        {
            return new Derived(std::forward<Args>(args)...);
        }
    };

    // ========================================================
    // 策略 2: 池化 Actor (PooledActor)
    // --------------------------------------------------------
    // 行为：从 ObjectPool 获取，归还给 ObjectPool。
    // 适用：Player, Monster, Bullet 等高频对象。
    // 模板参数：允许针对不同类型定制池子大小
    // ========================================================
    template <typename Derived, size_t PoolSize = 100000, size_t BatchSize = 128>
    class PooledActor : public Actor
    {
    public:
        // 继承构造函数
        using Actor::Actor;

        // 定义该类型专属的对象池
        using PoolType = ObjectPool<Derived, PoolSize, BatchSize>;

        // [实现契约] 销毁逻辑：归还给池子
        void finalize() override
        {
            // release 接受 T* 指针
            PoolType::instance().release(static_cast<Derived *>(this));
        }

        // [工厂方法] 必须通过此方法创建
        template <typename... Args>
        static Derived *create(Args &&...args)
        {
            auto ptr = PoolType::instance().acquire(std::forward<Args>(args)...);
            return ptr.release();
        }
    };

} // namespace aegis::core