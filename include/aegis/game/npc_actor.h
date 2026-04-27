/**
 * @file npc_actor.h
 * @brief Pooled NPC actor with BehaviorTree.CPP v4 AI integration.
 */
#pragma once

#include "aegis/core/actor_traits.h"
#include <behaviortree_cpp/bt_factory.h>

namespace aegis::core
{
    class SceneActor;
    // 基础 NPC Actor，支持内存池复用
    /**
     * @brief Pooled NPC actor with BehaviorTree.CPP AI.
     * 
     * AI is active only when players are within AOI range of the NPC.
     * SceneActor drives OnTick(), which triggers BT tick.
     * Pooled with 256 capacity, 64 batch size.
     */
    class NpcActor : public PooledActor<NpcActor, 256, 64>
    {
    public:
        explicit NpcActor(ActorID id) {}
        ~NpcActor() = default;

        // 基础初始化
        void reset(ActorID self_id, float x, float y);

        // 统一的消息处理入口
        void handle_message(ActorMessage *msg) override;

        // 由 SceneActor 统一驱动的 Tick
        void OnTick();

        // 坐标获取与设置
        float GetX() const { return x_; }
        float GetY() const { return y_; }
        void SetPos(float x, float y)
        {
            x_ = x;
            y_ = y;
        }
        void SetAiActive(bool active) { is_ai_active_ = active; }
        void SetScene(SceneActor *scene) { scene_ = scene; }
        SceneActor *GetScene() const { return scene_; }
        bool IsAiActive() const { return is_ai_active_; }

        // 行为树相关
        BT::Tree &GetBehaviorTree() { return tree_; }
        SceneActor *scene_ = nullptr;

        // AOI Grid 相关

        uint32_t get_aoi_grid_index() const { return aoi_grid_index_; }
        void set_aoi_grid_index(uint32_t index) { aoi_grid_index_ = index; }

    private:
        float x_ = 0.0f;
        float y_ = 0.0f;
        uint32_t aoi_grid_index_ = 0;

        // AI 行为树相关
        BT::Tree tree_;             // 修正为 v4 正确写法
        bool is_ai_active_ = false; // 用于后续的 AOI 唤醒/休眠标记
    };
}