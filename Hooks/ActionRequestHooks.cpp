#include "ActionRequestsHooks.hpp"
#include "../Combat/CombatSystem.hpp"

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

    std::atomic<bool> g_lightToHeavyTest{ false };

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

        const bool isPlayerAttack =
            caller == 0x142EFC750 &&
            stateName != nullptr &&
            std::strcmp(stateName, "Attack") == 0;

        if (isPlayerAttack &&
            g_lightToHeavyTest.load())
        {
            std::uint32_t replacement = 0;

            if (HJ::Combat::TryGetHeavyReplacement(
                packedCommand,
                replacement))
            {
                if (g_lightToHeavyTest.exchange(false))
                {
                    packedCommand =
                        replacement;

                    std::ostringstream stream;

                    stream
                        << "ATTACK OVERRIDE"
                        << " original=0x"
                        << std::hex
                        << originalPackedCommand
                        << " replacement=0x"
                        << replacement;

                    HJ::Logger::Info(
                        stream.str()
                    );
                }
            }
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

void HJ::Hooks::ActionRequest::TraceAttackObject(
    std::uintptr_t stateContainer,
    void* stateObject,
    const char* stateName,
    std::uintptr_t caller)
{
    if (stateObject == nullptr ||
        stateName == nullptr ||
        std::strcmp(stateName, "Attack") != 0 ||
        caller != 0x142F22305)
    {
        return;
    }

    const auto objectBytes =
        reinterpret_cast<std::uint8_t*>(
            stateObject
            );

    const std::uint32_t field78 =
        *reinterpret_cast<std::uint32_t*>(
            objectBytes + 0x78
            );

    const std::int32_t fieldB4 =
        *reinterpret_cast<std::int32_t*>(
            objectBytes + 0xB4
            );

    const std::uint8_t fieldC7 =
        *(objectBytes + 0xC7);

    std::ostringstream stream;

    stream
        << "ATTACK OBJECT"
        << " container=0x"
        << std::hex
        << stateContainer
        << " object=0x"
        << reinterpret_cast<std::uintptr_t>(
            stateObject
            )
        << " +78=0x"
        << field78
        << " +B4=0x"
        << fieldB4
        << " +C7=0x"
        << static_cast<unsigned int>(
            fieldC7
            );

    HJ::Logger::Info(
        stream.str()
    );
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

    void EnableLightToHeavyTest()
    {
        g_lightToHeavyTest.store(true);

        Logger::Info(
            "Next known player light attack will be replaced with heavy."
        );
    }
}