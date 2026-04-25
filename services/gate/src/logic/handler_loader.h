#pragma once

namespace aegis::gate
{
    /**
     * @brief Load all game logic message handlers (Login, Move, SkillCast, etc.)
     * into the global Dispatcher.
     */
    void load_handlers();
}
