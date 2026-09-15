#pragma once

#include <cstdint>

namespace HJ::Hooks::ActionRequest
{
    struct AttackRequest
    {
        std::uintptr_t container;
        std::uint32_t packedCommand;
        const char* stateName;
    };

    constexpr std::uint32_t GetCommandKey(
        std::uint32_t packedCommand)
    {
        return packedCommand & 0xFFFF;
    }

    constexpr std::uint32_t GetVariant(
        std::uint32_t packedCommand)
    {
        return packedCommand >> 16;
    }

    void TraceAttackObject(
        std::uintptr_t stateContainer,
        void* stateObject,
        const char* stateName,
        std::uintptr_t caller
    );

    constexpr std::uint32_t MakePackedCommand(
        std::uint16_t commandKey,
        std::uint16_t variant)
    {
        return
            static_cast<std::uint32_t>(commandKey) |
            (static_cast<std::uint32_t>(variant) << 16);
    }

    bool Initialize();

    void EnableLightToHeavyTest();

    bool RequestAttack(
        std::uint32_t packedCommand
    );
    
}