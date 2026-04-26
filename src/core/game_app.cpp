/**
 * @file game_app.cpp
 * @brief Implementation of GameApp singleton — manages RoomManager,
 * default scene, and initial NPC spawning.
 */
#include "aegis/core/game_app.h"
#include "aegis/core/room_manager.h"
#include "aegis/core/scene_actor.h"
#include "aegis/core/npc_actor.h"
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

        // 2. 创建默认主城 Scene
        default_scene_id_ = registry.create_actor<SceneActor>(map_width, map_height, cell_size);
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

            // 4. Spawn initial NPCs in the main city
            auto *concrete_scene = static_cast<SceneActor *>(scene);
            float center_x = map_width * 0.5f;
            float center_y = map_height * 0.5f;

            for (int i = 0; i < 3; ++i)
            {
                auto npc_id = registry.create_actor<NpcActor>();
                if (npc_id.is_valid())
                {
                    auto *npc = static_cast<NpcActor *>(registry.get(npc_id));
                    npc->reset(npc_id, center_x + i * 5.0f, center_y + i * 5.0f);
                    concrete_scene->AddNpc(npc);
                    Log::instance().info("[GameApp] Spawned Test NPC {} at ({}, {})",
                                         npc_id.raw, npc->GetX(), npc->GetY());
                }
            }
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
