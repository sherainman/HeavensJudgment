#include "Engine.hpp"

#include <Windows.h>
#include <cstdint>

namespace HJ::Engine
{
	namespace
	{
		constexpr std::uintptr_t kRuntimeStateGlobalRva = 0x0430BE48;
		constexpr std::uintptr_t kFightingOffset = 0x2E8;
	}

	bool TryGetFightingState(bool& outFighting)
	{
		const auto moduleBase =
			reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

		if (moduleBase == 0)
			return false;

		const auto runtimeState =
			*reinterpret_cast<const std::uintptr_t*>(
				moduleBase + kRuntimeStateGlobalRva);

		if (runtimeState == 0)
			return false;

		outFighting =
			*reinterpret_cast<const std::uint8_t*>(
				runtimeState + kFightingOffset) != 0;

		return true;
	}
}