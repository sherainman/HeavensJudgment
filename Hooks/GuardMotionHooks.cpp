#include "GuardMotionHooks.hpp"

#include "../Core/Logger.hpp"
#include "GuardDirectionHooks.hpp"

#include <Windows.h>
#include <safetyhook.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>

extern "C" std::uintptr_t HJ_GetPlayerCombatObject();

namespace
{
	SafetyHookInline g_guardCellHook{};

	// battle_motion_set cell getter. generic, so row and column both have to
	// match before anything is swapped.
	constexpr std::uintptr_t kGuardCellRva = 0x1E6670;

	// p_yagami_box is row 283.
	constexpr std::uint32_t kPlayerMotionRow = 283;
	constexpr std::uint32_t kGuardStartColumn = 0x08;
	constexpr std::uint32_t kGuardLoopColumn = 0x0A;
	constexpr std::uint32_t kGuardEndColumn = 0x0C;

	// the engine only reads st/lp/en once, when the GuardMiddle action starts,
	// so a live side change has to replay that entry to get a new loop.
	constexpr std::uintptr_t kGuardMiddleRva = 0x22745F0;
	constexpr std::uintptr_t kGuardOwnerOffset = 0x3C8;

	struct GuardSet
	{
		std::uint16_t start;
		std::uint16_t loop;
		std::uint16_t end;
	};

	constexpr GuardSet kLeftGuard{ 16319, 16317, 16315 };
	constexpr GuardSet kRightGuard{ 16328, 16321, 16320 };

	std::atomic<int> g_previousSide{ -2 };
	std::atomic<bool> g_rejectLogged{ false };

	bool TryReadColumnOffset(std::uintptr_t columnDesc,
		std::uint32_t& outOffset) noexcept
	{
		__try
		{
			outOffset = *reinterpret_cast<const std::uint32_t*>(
				columnDesc + 0x4);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	std::uint16_t __fastcall GuardCellHook(std::uintptr_t recordSet,
		std::uint32_t rowIndex, std::uintptr_t columnDesc)
	{
		const std::uint16_t original = g_guardCellHook.call<std::uint16_t>(
			recordSet, rowIndex, columnDesc);

		if (rowIndex != kPlayerMotionRow)
			return original;

		const int sideId = HJ::Hooks::GuardDirection::CurrentSideId();
		if (sideId != 1 && sideId != 2)
			return original;

		std::uint32_t column = 0;
		if (!TryReadColumnOffset(columnDesc, column))
			return original;

		const GuardSet& guard = sideId == 1 ? kLeftGuard : kRightGuard;

		switch (column)
		{
		case kGuardStartColumn:
			return guard.start;
		case kGuardLoopColumn:
			return guard.loop;
		case kGuardEndColumn:
			return guard.end;
		default:
			return original;
		}
	}
}

namespace HJ::Hooks::GuardMotion
{
	void Tick()
	{
		const int side = GuardDirection::CurrentSideId();
		const int previous = g_previousSide.load(std::memory_order_relaxed);

		if (side == previous)
			return;

		// guard went up or came down, the engine runs its own entry for those.
		if (previous < 0 || side < 0)
		{
			g_previousSide.store(side, std::memory_order_relaxed);
			return;
		}

		// a rejected push is usually just a busy frame, so keep trying.
		static ULONGLONG lastAttempt = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - lastAttempt < 150)
			return;
		lastAttempt = now;

		const std::uintptr_t combatObject = HJ_GetPlayerCombatObject();

		if (combatObject != 0 &&
			*reinterpret_cast<const std::uintptr_t*>(
				combatObject + kGuardOwnerOffset) != 0)
		{
			const auto base = reinterpret_cast<std::uintptr_t>(
				GetModuleHandleA(nullptr));
			const auto pushGuard = reinterpret_cast<bool(__fastcall*)(
				std::uintptr_t)>(base + kGuardMiddleRva);

			if (pushGuard(combatObject))
			{
				g_previousSide.store(side, std::memory_order_relaxed);
				g_rejectLogged.store(false, std::memory_order_relaxed);

				Logger::Info(std::format(
					"HJ GUARD: side {} -> {}", previous, side));
				return;
			}
		}

		if (!g_rejectLogged.exchange(true, std::memory_order_relaxed))
			Logger::Info("HJ GUARD: entry replay rejected, retrying.");
	}

	bool Initialize()
	{
		if (!kGuardMotionSwapEnabled)
		{
			Logger::Info("Guard motion swap disabled, hook parked.");
			return true;
		}

		const HMODULE gameModule = GetModuleHandleA(nullptr);
		if (!gameModule)
		{
			Logger::Error("Guard motion hook: no game module.");
			return false;
		}

		const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
		const auto target = base + kGuardCellRva;

		const std::uint8_t expected[] = {
			0x48, 0x8B, 0x41, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x25
		};
		if (std::memcmp(reinterpret_cast<void*>(target), expected,
			sizeof(expected)) != 0)
		{
			Logger::Error("Guard motion hook: bytes dont match.");
			return false;
		}

		g_guardCellHook = safetyhook::create_inline(
			reinterpret_cast<void*>(target),
			reinterpret_cast<void*>(&GuardCellHook));

		if (!g_guardCellHook)
		{
			Logger::Error("Guard motion hook: install failed.");
			return false;
		}

		Logger::Info("Guard motion hook installed.");
		return true;
	}
}
