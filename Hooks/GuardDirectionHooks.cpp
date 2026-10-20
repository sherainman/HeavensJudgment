#include "GuardDirectionHooks.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>
#include <safetyhook.hpp>
#include <Xinput.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>

extern "C" std::uintptr_t HJ_GetPlayerCombatObject();

namespace
{
	SafetyHookInline g_liveDispatchHook{};

	constexpr std::uintptr_t kLiveDispatchRva = 0x2EF15A0;
	constexpr std::uintptr_t kAngleRva = 0x2E84090;
	constexpr std::uintptr_t kSlotResolveRva = 0x2ED1270;

	
	constexpr float kCone = 1.047f;
	constexpr float kSideCone = 1.92f;
	constexpr float kFrontOverlap = 0.1f;
	constexpr int kNeutralBudget = 1;
	constexpr int kSideBudget = 3;

	enum class GuardSide
	{
		None,
		Both,
		Left,
		Right
	};

	std::atomic<int> g_bothBlocks{ 0 };
	std::atomic<int> g_holdSide{ -2 };
	std::atomic<int> g_blockCount{ 0 };
	std::atomic<unsigned> g_ownerMismatchCount{ 0 };
	std::atomic<unsigned> g_angleFailCount{ 0 };

	using SlotResolveFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
	using AngleFn = float*(__fastcall*)(std::uint64_t, float*, void*, void*);

	bool SafeReadU32(std::uintptr_t address, std::uint32_t& out) noexcept
	{
		__try
		{
			out = *reinterpret_cast<const std::uint32_t*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	GuardSide GetGuardSide() noexcept
	{
		XINPUT_STATE state{};

		if (XInputGetState(0, &state) != ERROR_SUCCESS)
			return GuardSide::None;

		// Circle alone holds neutral, Circle+LB left, Circle+RB right
		const bool b = (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
		if (!b)
			return GuardSide::None;

		const bool lb = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
		const bool rb = (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;

		if (lb && !rb)
			return GuardSide::Left;

		if (rb && !lb)
			return GuardSide::Right;

		return GuardSide::Both;
	}

	bool TryGetBlockAngle(std::uintptr_t owner, std::uintptr_t packet,
		float& outAngle) noexcept
	{
		const auto base =
			reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

		if (base == 0)
			return false;

		auto slotResolve = reinterpret_cast<SlotResolveFn>(base + kSlotResolveRva);
		auto angleFn = reinterpret_cast<AngleFn>(base + kAngleRva);

		if (!slotResolve || !angleFn)
			return false;

		std::uint32_t attacker = 0;
		if (!SafeReadU32(packet + 0x58, attacker) || attacker == 0)
			return false;

		__try
		{
			const std::uintptr_t context = slotResolve(owner + 0x30);
			if (context == 0)
				return false;

			const std::uintptr_t entity = *reinterpret_cast<const std::uintptr_t*>(context);
			if (entity == 0)
				return false;

			const std::uint32_t defender = *reinterpret_cast<const std::uint32_t*>(entity + 0xC);
			if (defender == 0)
				return false;

			std::uint32_t atk[2] = { attacker, 0 };
			std::uint64_t defBlob = static_cast<std::uint64_t>(defender);

			float angle = 0.0f;
			angleFn(defender, &angle, &defBlob, atk);
			if (!std::isfinite(angle))
				return false;

			outAngle = angle;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	std::uint8_t CallLiveDispatchOrig(std::uintptr_t owner, std::uintptr_t packet)
	{
		return g_liveDispatchHook.call<std::uint8_t>(owner, packet);
	}

	// allow/deny for the player guard path. everyone else is untouched
	// and falls straight through to the original.
	std::uint8_t __fastcall LiveDispatchHook(std::uintptr_t owner,
		std::uintptr_t packet)
	{
		const GuardSide side = GetGuardSide();

		const int sideId = side == GuardSide::None ? -1 :
			side == GuardSide::Both ? 0 :
			side == GuardSide::Left ? 1 : 2;
		if (sideId != g_holdSide.exchange(sideId))
			g_blockCount.store(0);

		if (side == GuardSide::None)
			return CallLiveDispatchOrig(owner, packet);

		const std::uintptr_t playerObject = HJ_GetPlayerCombatObject();
		if (playerObject == 0 || owner != playerObject)
		{
			const unsigned n = g_ownerMismatchCount.fetch_add(1) + 1;
			if (n <= 3 || n % 500 == 0)
			{
				HJ::Logger::Info(std::format(
					"HJ GUARD: non-player owner=0x{:X} player=0x{:X} (#{})",
					owner, playerObject, n));
			}
			return CallLiveDispatchOrig(owner, packet);
		}

		float angle = 0.0f;
		if (!TryGetBlockAngle(owner, packet, angle))
		{
			
			const unsigned n = g_angleFailCount.fetch_add(1) + 1;
			if (n <= 5 || n % 200 == 0)
			{
				HJ::Logger::Info(std::format(
					"HJ GUARD: angle resolve failed #{} owner=0x{:X} packet=0x{:X}",
					n, owner, packet));
			}
			return CallLiveDispatchOrig(owner, packet);
		}

		const float absAngle = std::fabs(angle);

		if (side == GuardSide::Both)
		{
			if (absAngle > kCone)
			{
				HJ::Logger::Info(std::format(
					"HJ GUARD: deny neutral cone angle={:.3f}", angle));
				return 0;
			}

			if (g_blockCount.load() >= kNeutralBudget)
			{
				HJ::Logger::Info("HJ GUARD: neutral guard broken");
				return 0;
			}

			const std::uint8_t result = CallLiveDispatchOrig(owner, packet);
			if (result != 0)
			{
				// neutral holds both sides but pops open right after
				const int n = g_bothBlocks.fetch_add(1) + 1;
				g_blockCount.fetch_add(1);
				HJ::Logger::Info(std::format(
					"HJ GUARD: neutral block angle={:.3f} count={}", angle, n));
			}
			return result;
		}

		const bool wantLeft = side == GuardSide::Left;

		if (g_blockCount.load() >= kSideBudget)
		{
			HJ::Logger::Info(std::format(
				"HJ GUARD: {} guard broken", wantLeft ? "left" : "right"));
			return 0;
		}

		if (absAngle <= kFrontOverlap)
		{
			const std::uint8_t frontResult = CallLiveDispatchOrig(owner, packet);
			if (frontResult != 0)
			{
				g_blockCount.fetch_add(1);
				HJ::Logger::Info(std::format(
					"HJ GUARD: front block on {}", wantLeft ? "left" : "right"));
			}
			return frontResult;
		}

		// +angle is a hit on the left, so a left hold must refuse
		// anything past the right edge of its cone
		if (wantLeft
			? (angle < -kFrontOverlap || angle > kSideCone)
			: (angle > kFrontOverlap || angle < -kSideCone))
		{
			HJ::Logger::Info(std::format(
				"HJ GUARD: deny side={} angle={:.3f}",
				wantLeft ? "L" : "R", angle));
			return 0;
		}

		const std::uint8_t result = CallLiveDispatchOrig(owner, packet);
		if (result != 0)
		{
			g_blockCount.fetch_add(1);
			HJ::Logger::Info(std::format(
				"HJ GUARD: block side={} angle={:.3f}",
				wantLeft ? "L" : "R", angle));
		}
		return result;
	}
}

namespace HJ::Hooks::GuardDirection
{
	bool Initialize()
	{
		if (!kDirectionalBlockEnabled)
		{
			Logger::Info("Directional block disabled, guard hooks parked.");
			return true;
		}

		const HMODULE gameModule = GetModuleHandleA(nullptr);
		if (!gameModule)
		{
			Logger::Error("GuardDirection hook: no game module.");
			return false;
		}

		const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
		const auto target = base + kLiveDispatchRva;

		const std::uint8_t expected[] = {
			0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55
		};
		if (std::memcmp(reinterpret_cast<void*>(target), expected,
			sizeof(expected)) != 0)
		{
			Logger::Error("Live dispatch hook: bytes dont match.");
			return false;
		}

		g_liveDispatchHook = safetyhook::create_inline(
			reinterpret_cast<void*>(target),
			reinterpret_cast<void*>(&LiveDispatchHook));

		if (!g_liveDispatchHook)
		{
			Logger::Error("Live dispatch hook: install failed.");
			return false;
		}

		Logger::Info("Live dispatch hook installed.");
		return true;
	}

	int CurrentSideId()
	{
		const GuardSide side = GetGuardSide();
		return side == GuardSide::None ? -1 :
			side == GuardSide::Left ? 1 :
			side == GuardSide::Right ? 2 : 0;
	}
}
