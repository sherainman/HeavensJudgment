#pragma once

#include <cstdint>

namespace HJ::Hooks::GuardDirection
{
	constexpr bool kDirectionalBlockEnabled = true;

	bool Initialize();

	// -1 none, 0 both, 1 left, 2 right
	int CurrentSideId();

	// publish the latest controller buttons; GetGuardSide reads this
	// instead of calling XInput on the per-dispatch path.
	void UpdateGuardButtons(std::uint16_t buttons) noexcept;
}
