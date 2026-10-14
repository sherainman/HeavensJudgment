#pragma once

#include <cstdint>

namespace HJ::HUD::RadialDisc
{
	// locates the record quadrants in the uiManager table (read-only
	// dump first), then drives them with native writes only.
	void Update(float health);

	void Initialize();

	void Report();

	bool IsReady();
}