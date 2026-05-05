/**
 * @file game_app.cpp
 * @brief Implementation of GameApp singleton — manages RoomManager,
 * default scene, and initial NPC spawning.
 */
#include "aegis/game/game_app.h"
#include "aegis/game/room_manager.h"
#include "aegis/game/scene_actor.h"
#include "aegis/game/npc_actor.h"
#include "aegis/common/aegisLog.h"
#include "aegis/core/scheduler.h" // for worker pinning
#include "aegis/core/actor_registry.h"

namespace aegis::core
{

    GameApp &GameApp::instance()
    {
        static GameApp app;
        return app;
    }

    void GameApp::init(float map_width, float map_height, float cell_size, int worker_id)
    {
        if (initialized_)
        {
            Log::instance().warn("[GameApp] Already initialized, skipping.");
            return;
        }

        auto &registry = ActorRegistry::instance();

        // 1. 创建全局 RoomManager
        room_manager_id_ = registry.create_actor<RoomManager>();
        auto *rm = registry.get(room_manager_id_);
        if (rm)
        {
            rm->set_worker_id(worker_id);
            Log::instance().info("[GameApp] RoomManager bound to Worker {}", worker_id);
        }
        Log::instance().info("[GameApp] RoomManager Created. ID: {}", room_manager_id_.raw);

        // 2. 创建默认主城 Scene（支持负坐标范围）
        float halfW = map_width * 0.5f;
        float halfH = map_height * 0.5f;
        default_scene_id_ = registry.create_actor<SceneActor>(-halfW, -halfH, halfW, halfH, cell_size);
        if (!default_scene_id_.is_valid())
        {
            Log::instance().critical("[GameApp] Failed to create Main City Scene!");
            throw std::runtime_error("Scene creation failed");
        }

        // 3. Pin scene to designated worker for tick affinity
        auto *scene = registry.get(default_scene_id_);
        if (scene)
        {
            scene->set_worker_id(worker_id);
            scene->set_parent_id(room_manager_id_);

            Log::instance().info("[GameApp] Main City Scene created. ID: {} (Worker {})",
                                 default_scene_id_.raw, worker_id);

            // NPC spawning disabled: test NPCs removed.
            // Monsters will be spawned by a proper wave system later.
        }
        else
        {
            Log::instance().critical("[GameApp] Scene created but get() returned nullptr!");
            throw std::runtime_error("Scene retrieval failed");
        }

        initialized_ = true;
        Log::instance().info("[GameApp] Bootstrap complete.");
    }

} // namespace aegis::core
