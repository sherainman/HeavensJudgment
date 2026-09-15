#include "Hooks.hpp"

#include "FighterCommandHooks.hpp"

#include "../Core/Logger.hpp"

namespace HJ::Hooks
{
    bool Initialize()
    {
        Logger::Info("Initializing hooks...");

        if (!FighterCommand::Initialize())
        {
            Logger::Error(
                "Failed to initialize FighterCommand hooks."
            );

            return false;
        }

        Logger::Info(
            "Hooks initialized successfully."
        );

        return true;
    }
}