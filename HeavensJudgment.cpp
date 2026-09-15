#include "HeavensJudgment.hpp"

#include "Core/Logger.hpp"
#include "Hooks/hooks.hpp"

#include <Windows.h>
#include <sstream>
#include <cstdint>

namespace HJ
{
    DWORD WINAPI Initialize(LPVOID parameter)
    {
        if (!Logger::Initialize())
            return 0;

        Logger::Info("Heaven's Judgment starting.");

        const HMODULE gameModule = GetModuleHandleA(nullptr);

        if (!gameModule)
        {
            Logger::Error("Failed to get main executable module.");
            return 0;
        }

        std::ostringstream stream;

        stream
            << "Main executable base: 0x"
            << std::hex
            << reinterpret_cast<std::uintptr_t>(gameModule);

        Logger::Info(stream.str());

        if (!Hooks::Initialize())
        {
            Logger::Error("Hook initialization failed.");
            return 0;
        }

        Logger::Info("Heaven's Judgment initialized successfully.");

        return 0;
    }
}