#pragma once

#include <cstdint>

namespace HJ::Hooks::GuardDirection
{
	constexpr bool kDirectionalBlockEnabled = true;

	bool Initialize();

	// -1 none, 0 both, 1 left, 2 right
	int CurrentSideId();
}
