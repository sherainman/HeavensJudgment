#include "CombatSystem.hpp"

#include "../Engine/Engine.hpp"

namespace HJ::Combat
{
    bool IsInCombat()
    {
        bool fighting = false;

        if (!HJ::Engine::TryGetFightingState(
            fighting))
        {
            return false;
        }

        return fighting;
    }
}