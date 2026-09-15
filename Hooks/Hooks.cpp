#include "Hooks.hpp"

#include "../Core/Logger.hpp"

#include <safetyhook.hpp>
#include <string>

namespace
{
    SafetyHookInline g_testHook{};

    __declspec(noinline) int TestFunction(int value)
    {
        return value + 10;
    }

    int TestFunctionHook(int value)
    {
        HJ::Logger::Info("SafetyHook detour executed.");

        const int originalResult =
            g_testHook.call<int>(value);

        return originalResult + 100;
    }
}

namespace HJ::Hooks
{
    bool Initialize()
    {
        Logger::Info("Initializing hooks...");

        // before installing the hook.
        const int before = TestFunction(5);

        Logger::Info(
            "Test function before hook: " +
            std::to_string(before)
        );

        g_testHook = safetyhook::create_inline(
            reinterpret_cast<void*>(&TestFunction),
            reinterpret_cast<void*>(&TestFunctionHook)
        );

        // after installing the hook.
        const int after = TestFunction(5);

        Logger::Info(
            "Test function after hook: " +
            std::to_string(after)
        );

        if (before != 15)
        {
            Logger::Error(
                "Unexpected pre-hook test result."
            );

            return false;
        }

        if (after != 115)
        {
            Logger::Error(
                "SafetyHook self-test failed."
            );

            return false;
        }

        Logger::Info(
            "SafetyHook self-test passed."
        );

        return true;
    }
}