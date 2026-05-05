/**
 * @file game_app.h
 * @brief Centralized game application bootstrap — replaces the business logic
 *        that was previously hard-coded into GateServer::init().
 *
 * GameApp is a singleton that owns:
 *   - RoomManager lifecycle (one instance)
 *   - Default main city scene creation
 *   - Global NPC spawning
 *   - Scene supervision parent linkage
 *
 * Once initialized, other components (e.g. handler_loader) call
 * GameApp::instance().default_scene_id() instead of the deleted
 * GateServer::g_DefaultSceneID.
 */
#pragma once

#include "aegis/core/actor_registry.h"

namespace aegis::core
{
    /**
     * @brief Singleton game logic bootstrap and world manager.
     *
     * Responsibilities:
     *   - Creates and owns the RoomManager singleton
     *   - Creates the default main city scene (camp)
     *   - Spawns initial NPCs in the main city
     *   - Provides default_scene_id() for login handler routing
     */
    class GameApp
    {
    public:
        static GameApp &instance();

        GameApp(const GameApp &) = delete;
        GameApp &operator=(const GameApp &) = delete;

        /**
         * @brief Bootstrap all game logic singletons and entities.
         * Must be called once after Scheduler is ready, before GateServer::run().
         *
         * Internally converts to [-halfW, halfW] x [-halfH, halfH] range.
         *
         * @param map_width   Width of the main city scene (default: 2000.0f)
         * @param map_height  Height of the main city scene (default: 2000.0f)
         * @param cell_size   AOI grid cell size (default: 256.0f)
         * @param worker_id   Target worker for the main city scene tick (default: 3)
         */
        void init(float map_width = 2000.0f,
                  float map_height = 2000.0f,
                  float cell_size = 256.0f,
                  int worker_id = 3);

        /**
         * @brief Get the default main city scene ActorID.
         */
        ActorID default_scene_id() const { return default_scene_id_; }

        /**
         * @brief Get the RoomManager ActorID.
         */
        ActorID room_manager_id() const { return room_manager_id_; }

        /**
         * @brief Check if the app has been initialized.
         */
        bool is_initialized() const { return initialized_; }

    private:
        GameApp() = default;
        ~GameApp() = default;

        bool initialized_ = false;
        ActorID room_manager_id_;
        ActorID default_scene_id_;
    };

} // namespace aegis::core
