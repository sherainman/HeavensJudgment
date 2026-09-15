#include "ActionRequestsHooks.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>
#include <safetyhook.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <sstream>

namespace
{
    SafetyHookInline g_attackBehaviorHook{};

    std::atomic<bool> g_tigerLightToHeavyTest{ false };

    void __fastcall AttackBehaviorHook(
        std::uintptr_t container,
        std::uint32_t packedCommand,
        const char* stateName)
    {
        const auto caller =
            reinterpret_cast<std::uintptr_t>(
                _ReturnAddress()
                );

        const std::uint32_t originalPackedCommand =
            packedCommand;

        if (g_tigerLightToHeavyTest.exchange(false) &&
            caller == 0x142EFC750 &&
            packedCommand == 0x00160042 &&
            stateName != nullptr &&
            std::strcmp(stateName, "Attack") == 0)
        {
            packedCommand =
                HJ::Hooks::ActionRequest::MakePackedCommand(
                    0x42,
                    0x52
                );

            HJ::Logger::Info(
                "ATTACK OVERRIDE: "
                "Tiger light 0x00160042 -> "
                "Tiger heavy 0x00520042"
            );
        }

        {
            const std::uint32_t commandKey =
                HJ::Hooks::ActionRequest::GetCommandKey(
                    packedCommand
                );

            const std::uint32_t variant =
                HJ::Hooks::ActionRequest::GetVariant(
                    packedCommand
                );

            std::ostringstream stream;

            stream
                << "ATTACK REQUEST"
                << " name=\""
                << (stateName ? stateName : "<null>")
                << "\""
                << " container=0x"
                << std::hex
                << container
                << " original=0x"
                << originalPackedCommand
                << " packed=0x"
                << packedCommand
                << " key=0x"
                << commandKey
                << " variant=0x"
                << variant
                << " caller=0x"
                << caller;

            HJ::Logger::Info(
                stream.str()
            );
        }

        g_attackBehaviorHook.call<void>(
            container,
            packedCommand,
            stateName
        );
    }
}

namespace HJ::Hooks::ActionRequest
{
    bool Initialize()
    {
        Logger::Info(
            "Initializing action request hooks..."
        );

        const HMODULE gameModule =
            GetModuleHandleA(nullptr);

        if (!gameModule)
        {
            Logger::Error(
                "ActionRequest hook: failed to get game module."
            );

            return false;
        }

        const auto base =
            reinterpret_cast<std::uintptr_t>(
                gameModule
                );

        //
        // StartAttackBehaviorFromPackedCommand
        // 0x142F21D80 - 0x140000000
        // = 0x02F21D80
        //
        constexpr std::uintptr_t AttackBehaviorRva =
            0x02F21D80;

        const auto target =
            base + AttackBehaviorRva;

        {
            std::ostringstream stream;

            stream
                << "Installing action request hook at 0x"
                << std::hex
                << target;

            Logger::Info(
                stream.str()
            );
        }

        g_attackBehaviorHook =
            safetyhook::create_inline(
                reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(
                    &AttackBehaviorHook
                    )
            );

        Logger::Info(
            "Action request hook installed."
        );

        return true;
    }

    void EnableTigerLightToHeavyTest()
    {
        g_tigerLightToHeavyTest.store(true);

        Logger::Info(
            "Next Tiger light attack will be replaced with heavy."
        );
    }
}