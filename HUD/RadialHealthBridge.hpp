#pragma once

#include <cstdint>


namespace HJ::HUD::RadialHealthBridge
{
	bool Initialize();

	float GetHealthRatio();

	bool IsReady();

	std::uintptr_t GetHealthHudController();
}
