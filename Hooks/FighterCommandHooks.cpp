#include "FighterCommandHooks.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>
#include <safetyhook.hpp>

#include <cstdint>
#include <intrin.h>
#include <sstream>

namespace
{
    SafetyHookInline g_checkEligibilityHook{};

    // addresses:
    // ulonglong FUN_142E8F500(
    //  ulonglong param_1,
    //  uint      param_2, // fighter command key
    //  uint      param_3, // style
    //  uint      param_4  // option/behavior fl;ag?
    // );
    // 
    // 
    // style fightercommand key:
    // snake = 0x40
    // crane = 0x41
    // tiger = 0x42
    // boxer = 0x73
    // 
    // stores at:
    // DAT_14430BE48 + 0xE8 
    // 
    // windosx x64 
    // rcx = context
    // edx = key
    // r8d = mode
    // r9b = option
    //
    std::uint64_t __fastcall CheckEligibilityHook(
        std::uint64_t context,
        std::uint32_t key,
        std::uint32_t mode,
        std::uint8_t option)
    {
        const auto caller =
            reinterpret_cast<std::uintptr_t>(
                _ReturnAddress()
                );

        // run the og de function first so it can record its eligibility result
        const std::uint64_t result =
            g_checkEligibilityHook.call<std::uint64_t>(
                context,
                key,
                mode,
                option
            );

        
        const bool eligible =
            (result & 0xFF) != 0;

       
        // function casn execute OOOFFFTEEENNNN only log when something about the check changes on this thread

        thread_local bool hasPrevious = false;
        thread_local std::uint32_t previousKey = 0;
        thread_local std::uint32_t previousMode = 0;
        thread_local std::uint8_t previousOption = 0;
        thread_local std::uintptr_t previousCaller = 0;
        thread_local bool previousEligible = false;

        const bool changed =
            !hasPrevious ||
            key != previousKey ||
            mode != previousMode ||
            option != previousOption ||
            caller != previousCaller ||
            eligible != previousEligible;

        if (changed)
        {
            std::ostringstream stream;

            stream
                << "FighterCommand eligibility:"
                << " key=0x"
                << std::hex
                << key
                << " mode=0x"
                << mode
                << " option=0x"
                << static_cast<unsigned int>(option)
                << " caller=0x"
                << caller
                << " result="
                << (eligible ? "true" : "false");

            HJ::Logger::Info(stream.str());

            hasPrevious = true;
            previousKey = key;
            previousMode = mode;
            previousOption = option;
            previousCaller = caller;
            previousEligible = eligible;
        }

        // dont alter the result during observation ever
        return result;
    }
}

namespace HJ::Hooks::FighterCommand
{
    bool Initialize()
    {
        const HMODULE gameModule =
            GetModuleHandleA(nullptr);

        if (!gameModule)
        {
            Logger::Error(
                "FighterCommand hook: failed to get game module."
            );

            return false;
        }

        const auto base =
            reinterpret_cast<std::uintptr_t>(
                gameModule
                );

        //
        // TEMPORARY hardcoded rva
        // 0x142E8F500 - 0x140000000
        // = 0x02E8F500
        //
        constexpr std::uintptr_t CheckEligibilityRva =
            0x02E8F500;

        const auto target =
            base + CheckEligibilityRva;

        {
            std::ostringstream stream;

            stream
                << "Installing FighterCommand eligibility hook at 0x"
                << std::hex
                << target;

            Logger::Info(stream.str());
        }

        g_checkEligibilityHook =
            safetyhook::create_inline(
                reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(
                    &CheckEligibilityHook
                    )
            );

        Logger::Info(
            "FighterCommand eligibility hook installed."
        );

        return true;
    }
}