#include "FighterCommandHooks.hpp"
#include "ActionRequestsHooks.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>
#include <safetyhook.hpp>

#include <cstdint>
#include <intrin.h>
#include <sstream>
#include <atomic>

#include <mutex>
#include <unordered_set>

namespace
{
    SafetyHookInline g_checkEligibilityHook{};
    SafetyHookInline g_namedStateHook{};

    std::atomic<bool> g_f6WasDown{ false };
    std::atomic<ULONGLONG> g_traceUntil{ 0 };
    std::atomic<ULONGLONG> g_traceStartedAt{ 0 };

    constexpr ULONGLONG TraceDurationMs = 2000;

    struct TraceEntry
    {
        std::uint32_t key;
        std::uint32_t mode;
        std::uint8_t option;
        std::uintptr_t caller;
        bool eligible;

        bool operator==(const TraceEntry& other) const
        {
            return
                key == other.key &&
                mode == other.mode &&
                option == other.option &&
                caller == other.caller &&
                eligible == other.eligible;
        }
    };

    struct TraceEntryHash
    {
        std::size_t operator()(const TraceEntry& entry) const
        {
            std::size_t hash = entry.key;

            hash ^= static_cast<std::size_t>(entry.mode) << 8;
            hash ^= static_cast<std::size_t>(entry.option) << 16;
            hash ^= entry.caller;
            hash ^= static_cast<std::size_t>(entry.eligible) << 24;

            return hash;
        }
    };

    std::mutex g_traceMutex;

    std::unordered_set<
        TraceEntry,
        TraceEntryHash
    > g_traceEntries;

    void UpdateTraceHotkey()
    {
        const bool f6Down =
            (GetAsyncKeyState(VK_F6) & 0x8000) != 0;

        const bool wasDown =
            g_f6WasDown.exchange(f6Down);

        if (f6Down && !wasDown)
        {
            const ULONGLONG now =
                GetTickCount64();

            g_traceStartedAt.store(now);

            g_traceUntil.store(
                now + TraceDurationMs
            );

            HJ::Hooks::ActionRequest::EnableLightToHeavyTest();

            {
                std::scoped_lock lock(g_traceMutex);
                g_traceEntries.clear();
            }

            HJ::Logger::Info(
                "=== COMBAT TRACE STARTED: "
                "2 second capture window ==="
            );
        }
    }

    bool IsTraceActive()
    {
        const ULONGLONG until =
            g_traceUntil.load();

        if (until == 0)
            return false;

        const ULONGLONG now =
            GetTickCount64();

        if (now <= until)
            return true;

        ULONGLONG expected = until;

        if (g_traceUntil.compare_exchange_strong(
            expected,
            0))
        {
            HJ::Logger::Info(
                "=== COMBAT TRACE FINISHED ==="
            );
        }

        return false;
    }

    DWORD WINAPI TraceHotkeyThread(LPVOID)
    {
        HJ::Logger::Info(
            "Combat trace hotkey ready. Press F6 to capture."
        );

        while (true)
        {
            UpdateTraceHotkey();

            // This also ensures the FINISHED message appears
            // even if no eligibility calls happen near the end.
            IsTraceActive();

            Sleep(10);
        }

        return 0;
    }

    void* __fastcall NamedStateHook(
        std::uintptr_t stateContainer,
        void* stateObject,
        const char* stateName)
    {
        const auto caller =
            reinterpret_cast<std::uintptr_t>(
                _ReturnAddress()
                );

        if (IsTraceActive())
        {
            std::ostringstream stream;

            stream
                << "STATE TRACE"
                << " name=\""
                << (stateName ? stateName : "<null>")
                << "\""
                << " container=0x"
                << std::hex
                << stateContainer
                << " object=0x"
                << reinterpret_cast<std::uintptr_t>(
                    stateObject
                    )
                << " caller=0x"
                << caller;

            HJ::Hooks::ActionRequest::TraceAttackObject(
                stateContainer,
                stateObject,
                stateName,
                caller
            );

            HJ::Logger::Info(
                stream.str()
            );
        }

        return g_namedStateHook.call<void*>(
            stateContainer,
            stateObject,
            stateName
        );
    }

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
        const bool traceActive =
            IsTraceActive();

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

        if (traceActive)
        {
            const TraceEntry entry{
                key,
                mode,
                option,
                caller,
                eligible
            };

            bool firstOccurrence = false;

            {
                std::scoped_lock lock(g_traceMutex);

                firstOccurrence =
                    g_traceEntries.insert(entry).second;
            }

            if (firstOccurrence)
            {
                const ULONGLONG now =
                    GetTickCount64();

                const ULONGLONG elapsed =
                    now - g_traceStartedAt.load();

                std::ostringstream stream;

                stream
                    << "TRACE +"
                    << std::dec
                    << elapsed
                    << "ms"
                    << " thread="
                    << GetCurrentThreadId()
                    << " context=0x"
                    << std::hex
                    << context
                    << " key=0x"
                    << key
                    << " mode=0x"
                    << mode
                    << " option=0x"
                    << static_cast<unsigned int>(option)
                    << " caller=0x"
                    << caller
                    << " result="
                    << (eligible ? "true" : "false");

                HJ::Logger::Info(
                    stream.str()
                );
            }
        }

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

        //
        // FUN_1403B43D0
        // 0x1403B43D0 - 0x140000000
        // = 0x003B43D0
        //
        constexpr std::uintptr_t NamedStateRva =
            0x003B43D0;

        const auto target =
            base + CheckEligibilityRva;

        const auto namedStateTarget =
            base + NamedStateRva;

        {
            std::ostringstream stream;

            stream
                << "Installing FighterCommand eligibility hook at 0x"
                << std::hex
                << target;

            Logger::Info(
                stream.str()
            );
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

        {
            std::ostringstream stream;

            stream
                << "Installing named state hook at 0x"
                << std::hex
                << namedStateTarget;

            Logger::Info(
                stream.str()
            );
        }

        g_namedStateHook =
            safetyhook::create_inline(
                reinterpret_cast<void*>(
                    namedStateTarget
                    ),
                reinterpret_cast<void*>(
                    &NamedStateHook
                    )
            );

        Logger::Info(
            "Named state hook installed."
        );

        HANDLE traceThread =
            CreateThread(
                nullptr,
                0,
                &TraceHotkeyThread,
                nullptr,
                0,
                nullptr
            );

        if (!traceThread)
        {
            Logger::Error(
                "Failed to create combat trace hotkey thread."
            );

            return false;
        }

        CloseHandle(traceThread);

        return true;
    }
}