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

    constexpr std::uint32_t MakePackedCommand(
        std::uint16_t commandKey,
        std::uint16_t variant)
    {
        return
            static_cast<std::uint32_t>(commandKey) |
            (static_cast<std::uint32_t>(variant) << 16);
    }

    bool Initialize();

    void EnableTigerLightToHeavyTest();
}