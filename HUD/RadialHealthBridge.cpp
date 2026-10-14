#include "RadialHealthBridge.hpp"

#include "RadialDisc.hpp"
#include "RadialDiscDrive.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>

//
// Health capture only for the frameoverlay
//

namespace
{
	SafetyHookMid g_healthHook{};

	std::uintptr_t g_moduleBase = 0;

	std::atomic<float> g_healthRatio{ 1.0f };

	std::atomic<bool> g_healthReady{ false };

	std::atomic<std::uintptr_t>
		g_healthHudController{ 0 };

	constexpr std::uintptr_t kHealthUpdateRva =
		0x0275D3D3;

	void HealthUpdateHook(
		SafetyHookContext& context)
	{
		float health =
			context.xmm6.f32[0];


		if (!std::isfinite(health))
			return;


		if (health < 0.0f)
			health = 0.0f;

		if (health > 1.0f)
			health = 1.0f;


		g_healthRatio.store(
			health,
			std::memory_order_relaxed
		);

		g_healthReady.store(
			true,
			std::memory_order_relaxed
		);


		g_healthHudController.store(
			context.rdi,
			std::memory_order_relaxed
		);

	}
}


namespace HJ::HUD::RadialHealthBridge
{
	bool Initialize()
	{
		g_moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr)
			);


		if (!g_moduleBase)
		{
			HJ::Logger::Error(
				"[RADIAL HP] no game module"
			);

			return false;
		}


		const std::uintptr_t healthTarget =
			g_moduleBase + kHealthUpdateRva;

		const auto* healthBytes =
			reinterpret_cast<const std::uint8_t*>(healthTarget);

		if (
			healthBytes[0] != 0x48 ||
			healthBytes[1] != 0x8D ||
			healthBytes[2] != 0x87 ||
			healthBytes[3] != 0x90 ||
			healthBytes[4] != 0x01 ||
			healthBytes[5] != 0x00 ||
			healthBytes[6] != 0x00
			)
		{
			HJ::Logger::Error(
				"[RADIAL HP] health update address didnt match"
			);

			return false;
		}

		g_healthHook =
			safetyhook::create_mid(
				reinterpret_cast<void*>(healthTarget),
				&HealthUpdateHook
			);

		if (!g_healthHook)
		{
			HJ::Logger::Error(
				"[RADIAL HP] health hook failed"
			);

			return false;
		}

		HJ::Logger::Info("[RADIAL HP] health source ready");

		HJ::HUD::RadialDisc::Initialize();

		HJ::HUD::RadialDiscDrive::Initialize();

		return true;
	}


	float GetHealthRatio()
	{
		return g_healthRatio.load(
			std::memory_order_relaxed
		);
	}


	bool IsReady()
	{
		return g_healthReady.load(
			std::memory_order_relaxed
		);
	}


	std::uintptr_t GetHealthHudController()
	{
		return g_healthHudController.load(
			std::memory_order_relaxed
		);
	}
}
