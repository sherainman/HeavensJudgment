#define NOMINMAX
#pragma comment(lib, "Xinput.lib")

#include "FighterCommandHooks.hpp"
#include "ActionRequestsHooks.hpp"
#include "GuardDirectionHooks.hpp"
#include "GuardMotionHooks.hpp"

#include "../Core/Logger.hpp"
#include "../Engine/Engine.hpp"

#include <Windows.h>
#include <safetyhook.hpp>
#include <Xinput.h>

#include <algorithm>
#include <cstdint>
#include <intrin.h>
#include <sstream>
#include <atomic>
#include <string>
#include <cstring>
#include <format>

#include <mutex>
#include <unordered_set>

extern "C" std::uintptr_t HJ_GetPlayerCombatObject();

// i am going to make an effort to comment this.
// i am going to make an effort to comment this. yadda yadda

namespace
{
	using HJAction =
		HJ::Hooks::FighterCommand::HJAction;

	SafetyHookInline g_checkEligibilityHook{};
	SafetyHookInline g_namedStateHook{};
	SafetyHookInline g_controllerTranslateHook{};
	SafetyHookInline g_processHitDamageHook{};
	SafetyHookInline g_damageStateInitHook{};
	SafetyHookInline g_enemyDamageDispatchHook{};

	std::atomic<bool> g_f6WasDown{ false };
	std::atomic<ULONGLONG> g_traceUntil{ 0 };
	std::atomic<ULONGLONG> g_traceStartedAt{ 0 };
	std::atomic<bool> g_lockOnEnabled{ false };
	std::atomic<ULONGLONG>
		g_lastPlayerCombatUpdateAt{ 0 };
	std::atomic<bool> g_rightHandTransportOn{ false };

	std::atomic<bool> g_tackleActive{ false };
	std::atomic<std::uint64_t> g_tackleSequence{ 0 };
	std::atomic<ULONGLONG> g_tackleStartedAt{ 0 };

	constexpr ULONGLONG
		kGameplayInputFreshnessMs = 100;

	constexpr ULONGLONG TraceDurationMs = 12000;
	std::atomic<std::uint64_t> g_attackTraceSerial{ 0 };
	std::atomic<std::uint64_t> g_transportTraceSequence{ 0 };

	struct BufferedActionState
	{
		std::atomic<HJAction> action{
			HJAction::None
		};

		std::atomic<HJAction> offeredAction{
			HJAction::None
		};

		std::atomic<std::uint64_t> sequence{
			0
		};

		std::atomic<ULONGLONG> queuedAt{
			0
		};

		std::atomic<ULONGLONG> expiresAt{
			0
		};

		std::atomic<bool> transportOn{
			false
		};

		std::atomic<std::uint32_t> sourceCommand{
			0
		};
	};

	BufferedActionState g_buffer{};

	std::atomic<std::uint64_t>
		g_nextBufferSequence{ 0 };
	std::atomic<std::uint32_t>
		g_currentAttackCommand{ 0 };

	std::atomic<std::uintptr_t>
		g_currentAttackContainer{ 0 };

	// stun latch: stun mode owns the player container when active
	std::atomic<bool>
		g_playerStunned{ false };

	std::atomic<ULONGLONG>
		g_playerStunnedAt{ 0 };

	constexpr ULONGLONG kStunLatchExpiryMs =
		3000;

	// test for attack steering / commitment

	std::atomic<ULONGLONG>
		g_currentAttackStartedAt{ 0 };

	constexpr std::uint32_t kHJRightHand1 =
		0x00010073;

	constexpr ULONGLONG kActionBufferMs =
		400;

	struct ShoulderState
	{
		bool rb = false;
		bool lb = false;
		bool rt = false;
		bool lt = false;
	};

	ShoulderState g_previousShoulders{};

	// analog triggers bounce around the digital threshold on hard
	// shoulder presses, demand straight frames before believing one
	constexpr int kTriggerDebounceFrames = 3;
	int g_rtStableFrames = 0;
	int g_ltStableFrames = 0;
	bool g_prevRawRT = false;
	bool g_prevRawLT = false;

	enum class PendingTrigger : std::uint8_t
	{
		None,
		RightLeg,
		LeftLeg
	};

	PendingTrigger g_pendingTrigger =
		PendingTrigger::None;

	ULONGLONG g_pendingTriggerStart = 0;
	std::uint64_t g_pendingTriggerSequence = 0;
	std::uint64_t g_pendingTriggerCancelSequence = 0;
	std::atomic<std::uint64_t> g_triggerCancelSequence{ 0 };

	// Keep a short two-trigger tackle chord without delaying root kicks five frames.
	constexpr ULONGLONG kTriggerChordWindowMs = 50;

	// Dragon Engine resets thezse clock scales back to 1f at the top of every frame after clamping to >=0.02f
	// channel 0 = world/physics, channel 1 = player, channel 2 = vfx/particles
	constexpr std::uintptr_t kEngineWorldTimeScaleRva = 0x05313460;
	constexpr std::uintptr_t kEnginePlayerTimeScaleRva = 0x05313464;
	constexpr std::uintptr_t kEngineVfxTimeScaleRva = 0x05313468;

	struct HitstopState
	{
		std::atomic<std::uint64_t> sequence{ 0 };
		ULONGLONG startedAt = 0;
		float holdMs = 0.0f;
		float releaseMs = 0.0f;
		float intensity = 0.0f;
		float dirX = 0.0f;
		bool playerBlock = false;
	};

	HitstopState g_hitstop{};

	enum class EnemyReactionKind : std::uint8_t
	{
		None,
		StandardHit,
		HeavyKnockdown,
		BlockedHit
	};

	struct TraceEntry
	{
		std::uint32_t key;
		std::uint32_t mode;
		std::uint8_t option;
		std::uintptr_t caller;
		bool eligible;
		std::uint64_t attackSerial;

		bool operator==(const TraceEntry& other) const
		{
			return
				key == other.key &&
				mode == other.mode &&
				option == other.option &&
				caller == other.caller &&
				eligible == other.eligible &&
				attackSerial == other.attackSerial;
		}
	};

	struct TraceEntryHash
	{
		std::size_t operator()(const TraceEntry& entry) const
		{
			std::size_t hash = entry.key;

			hash ^= static_cast<std::size_t>(entry.mode) << 8;
			hash ^= static_cast<std::size_t>(entry.option) << 16;
			hash ^= entry.caller;
			hash ^= static_cast<std::size_t>(entry.eligible) << 24;
			hash ^= static_cast<std::size_t>(entry.attackSerial) << 1;

			return hash;
		}
	};

	std::mutex g_traceMutex;

	std::unordered_set<
		TraceEntry,
		TraceEntryHash
	> g_traceEntries;

	void UpdateTraceHotkey()
	{
		const bool f6Down =
			(GetAsyncKeyState(VK_F6) & 0x8000) != 0;

		const bool wasDown =
			g_f6WasDown.exchange(f6Down);

		if (f6Down && !wasDown)
		{
			const ULONGLONG now =
				GetTickCount64();

			g_traceStartedAt.store(now);
			g_transportTraceSequence.store(0);

			g_traceUntil.store(
				now + TraceDurationMs
			);

			{
				std::scoped_lock lock(g_traceMutex);
				g_traceEntries.clear();
			}

			HJ::Logger::Info(
				"=== COMBAT TRACE STARTED: "
				"12 second capture window ==="
			);
		}
	}

	void UpdateEngineFightingDiagnostic()
	{
		static bool initialized = false;
		static bool previousFighting = false;

		bool fighting = false;

		if (!HJ::Engine::TryGetFightingState(fighting))
			return;

		if (!initialized)
		{
			initialized = true;
			previousFighting = fighting;

			HJ::Logger::Info(
				fighting
				? "HJ ENGINE FIGHTING initial: true"
				: "HJ ENGINE FIGHTING initial: false");

			return;
		}

		if (fighting == previousFighting)
			return;

		HJ::Logger::Info(
			fighting
			? "HJ ENGINE FIGHTING: false -> true"
			: "HJ ENGINE FIGHTING: true -> false");

		previousFighting = fighting;
	}

	const char* GetHJActionName(
		HJAction action)
	{
		switch (action)
		{
		case HJAction::RightHand:
			return "RightHand";

		case HJAction::LeftHand:
			return "LeftHand";

		case HJAction::RightLeg:
			return "RightLeg";

		case HJAction::LeftLeg:
			return "LeftLeg";

		case HJAction::Tackle:
			return "Tackle";

		default:
			return "None";
		}
	}

	// just obscervations no input, eligibility or state behavior is changed here.
	void TraceCadence(const char* event, std::uint32_t detail = 0)
	{
		const ULONGLONG now = GetTickCount64();
		if (now > g_traceUntil.load() || g_traceUntil.load() == 0)
			return;
		const ULONGLONG started = g_currentAttackStartedAt.load();
		const ULONGLONG queued = g_buffer.queuedAt.load();
		HJ::Logger::Info(std::format(
			"CADENCE +{}ms event={} detail=0x{:X} source=0x{:X} "
			"container=0x{:X} attack_age={}ms sequence={} action={} buffer_age={}ms",
			now - g_traceStartedAt.load(), event, detail,
			g_currentAttackCommand.load(), g_currentAttackContainer.load(),
			started && now >= started ? now - started : 0,
			g_buffer.sequence.load(), GetHJActionName(g_buffer.action.load()),
			queued && now >= queued ? now - queued : 0));
	}

	HJAction GetCommandInputAction(
		std::uint32_t packedCommand)
	{
		const std::uint32_t key =
			packedCommand & 0xFFFF;

		if (key != 0x73)
			return HJAction::None;

		const std::uint32_t variant =
			packedCommand >> 16;

		switch (variant)
		{
			// Right Hand
		case 0x01: // RH1
		case 0x02: // RH2
		case 0x03: // RH3
		case 0x04: // RH4
		case 0x14: // RunRH1
		case 0x15: // RunRH2
		case 0x1E: // LH1 -> RH
		case 0x23: // LK1 -> RH
		case 0x29: // DownRH1
		case 0x2C: // EvadeRH
		case 0x34: // RH1_LK -> RH
		case 0x37: // RH1_LH -> RH
		case 0x38: // RH1_RK -> RH
		case 0x3C: // RK2 -> RH
			return HJAction::RightHand;

			// Left Hand
		case 0x05: // LH1
		case 0x06: // LH2
		case 0x07: // LH3
		case 0x08: // LH4
		case 0x0D: // RH1 -> LH branch
		case 0x12: // RH1_RK -> LH
		case 0x1B: // RK1 -> LH
		case 0x26: // RunLH1
		case 0x2A: // DownLH1
		case 0x2D: // EvadeLH
		case 0x31: // LK1_RH -> LH
		case 0x32: // RH2 -> LH
		case 0x35: // LH1_RH -> LH
		case 0x3A: // LH1_RK -> LH
		case 0x3B: // RK1_LK -> LH
			return HJAction::LeftHand;

			// Right Leg
		case 0x09: // RK1
		case 0x0A: // RK2
		case 0x0E: // RH1_LH -> RK
		case 0x11: // RH1 -> RK
		case 0x1A: // DownRK1
		case 0x1F: // LH1 -> RK
		case 0x27: // RunRK1
		case 0x2E: // EvadeRK
		case 0x33: // RH2_LH -> RK
		case 0x39: // LH2 -> RK
		case 0x25: // LK1 -> RK
			return HJAction::RightLeg;

			// Left Leg/kicks/kneeswhatever
		case 0x0C: // LK1
		case 0x1C: // LK2
		case 0x1D: // LK3
		case 0x0F: // RH1_LH -> LK
		case 0x10: // RH1 -> LK
		case 0x13: // RH1_RK -> LK
		case 0x20: // LH1 -> LK
		case 0x28: // RunLK1
		case 0x2B: // DownLK1
		case 0x2F: // EvadeLK
		case 0x36: // RH2 -> LK
		case 0x22: // RK1 -> LK
		case 0x3D: // RK2 -> LK
			return HJAction::LeftLeg;

		case 0x16: // TackleAcquire
			return HJAction::Tackle;

			// auto transitions etc.
			// LH4, tackle mount/ground pound/etc.
		default:
			return HJAction::None;
		}
	}

	void ClearBufferedHJAction()
	{
		g_buffer.action.store(
			HJAction::None
		);

		g_buffer.offeredAction.store(
			HJAction::None
		);

		g_buffer.sourceCommand.store(0);
		g_buffer.sequence.store(0);
		g_buffer.queuedAt.store(0);
		g_buffer.expiresAt.store(0);
		g_buffer.transportOn.store(false);
	}

	void QueueHJAction(
		HJAction action)
	{
		if (action == HJAction::None)
			return;

		const ULONGLONG now =
			GetTickCount64();

		const std::uint64_t sequence =
			g_nextBufferSequence.fetch_add(1) + 1;

		const HJAction previousAction =
			g_buffer.action.load();

		const std::uint64_t previousSequence =
			g_buffer.sequence.load();

		g_buffer.action.store(
			action
		);

		g_buffer.sequence.store(
			sequence
		);

		g_buffer.queuedAt.store(
			now
		);

		g_buffer.expiresAt.store(
			now + kActionBufferMs
		);

		// first transport update should be ON!!!!!
		g_buffer.transportOn.store(
			false
		);

		g_buffer.offeredAction.store(
			HJAction::None
		);

		g_buffer.sourceCommand.store(
			g_currentAttackCommand.load()
		);
		TraceCadence("buffer-queued", static_cast<std::uint32_t>(action));

		if (previousAction == HJAction::None)
		{
			HJ::Logger::Info(
				std::format(
					"HJ BUFFER: queued #{} {}",
					sequence,
					GetHJActionName(action)
				)
			);
		}
		else if (previousAction == action)
		{
			HJ::Logger::Info(
				std::format(
					"HJ BUFFER: refreshed #{} -> #{} {}",
					previousSequence,
					sequence,
					GetHJActionName(action)
				)
			);
		}
		else
		{
			HJ::Logger::Info(
				std::format(
					"HJ BUFFER: replaced #{} {} -> #{} {}",
					previousSequence,
					GetHJActionName(previousAction),
					sequence,
					GetHJActionName(action)
				)
			);
		}
	}

	void EmitHJAction(
		HJAction action,
		std::uint32_t* buttonMask,
		std::uint8_t* buttonValues)
	{
		if (!buttonMask || !buttonValues)
			return;

		__try
		{
			switch (action)
			{
			case HJAction::RightHand:
				// virtual R2 token
				*buttonMask |= 0x20;
				buttonValues[5] = 0xFF;
				g_rightHandTransportOn.store(true);
				break;

			case HJAction::LeftHand:
				// virtual Triangle
				*buttonMask |= 0x08;
				buttonValues[3] = 0xFF;
				break;

			case HJAction::RightLeg:
				// virtual Square
				*buttonMask |= 0x04;
				buttonValues[2] = 0xFF;
				break;

			case HJAction::LeftLeg:
				// virtual L2 transport token
				*buttonMask |= 0x10;
				buttonValues[4] = 0xFF;
				break;

			case HJAction::Tackle:
				// virtual Circle
				*buttonMask |= 0x02;
				buttonValues[1] = 0xFF;
				break;

			default:
				break;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	bool HasBufferedFollowUp(
		std::uint32_t packedCommand,
		HJAction action)
	{
		if ((packedCommand & 0xFFFF) != 0x73)
			return false;

		const std::uint32_t variant =
			packedCommand >> 16;

		// completed manual branches in p_yagami_box
		switch (action)
		{
		case HJAction::RightHand:
			switch (variant)
			{
			case 0x01:
			case 0x02:
			case 0x03:
			case 0x05:
			case 0x0C:
			case 0x0E:
			case 0x10:
			case 0x12:
			case 0x14:
			case 0x1C:
			case 0x0D:
			case 0x11:
			case 0x36:
			case 0x39:
			case 0x0A: // RK2 -> RH
			case 0x0B: // RK3 -> RH
			case 0x2C: // Sway RH -> hand string
			case 0x2D: // Sway LH -> hand string
			case 0x2E: // Sway RK -> hand string
			case 0x2F: // Sway LK -> hand string
				return true;
			}
			break;

		case HJAction::LeftHand:
			switch (variant)
			{
			case 0x01:
			case 0x02:
			case 0x03:
			case 0x05:
			case 0x06:
			case 0x09:
			case 0x10:
			case 0x11:
			case 0x1E:
			case 0x23:
			case 0x38:
			case 0x1F: // LH1_RK -> LH
			case 0x22: // RK1_LK -> LH
			case 0x25: // LK1_RK -> RK1_LH
			case 0x2C: // Sway RH -> hand string
			case 0x2D: // Sway LH -> hand string
			case 0x2E: // Sway RK -> hand string
			case 0x2F: // Sway LK -> hand string
				return true;
			}
			break;

		case HJAction::RightLeg:
			switch (variant)
			{
			case 0x01:
			case 0x05:
			case 0x09:
			case 0x0D:
			case 0x32:
			case 0x35:
			case 0x06:
			case 0x36:
			case 0x0A:
			case 0x2C: // Sway RH -> kick string
			case 0x2D: // Sway LH -> kick string
			case 0x2E: // Sway RK -> kick string
			case 0x2F: // Sway LK -> kick string
				return true;
			}
			break;

		case HJAction::LeftLeg:
			switch (variant)
			{
			case 0x01:
			case 0x05:
			case 0x0C:
			case 0x0D:
			case 0x11:
			case 0x12:
			case 0x1C:
			case 0x23:
			case 0x02:
			case 0x38:
			case 0x39:
			case 0x09:
			case 0x0A: // RK2 -> LK
			case 0x2C: // Sway RH -> kick string
			case 0x2D: // Sway LH -> kick string
			case 0x2E: // Sway RK -> kick string
			case 0x2F: // Sway LK -> kick string
				return true;
			}
			break;
		}

		return false;
	}

	void UpdateBufferedHJAction(
		std::uint32_t* buttonMask,
		std::uint8_t* buttonValues)
	{
		const HJAction action =
			g_buffer.action.load();

		if (action == HJAction::None)
		{
			g_buffer.offeredAction.store(
				HJAction::None
			);

			return;
		}

		const ULONGLONG now =
			GetTickCount64();

		const ULONGLONG expiresAt =
			g_buffer.expiresAt.load();

		const std::uint32_t sourceCommand =
			g_buffer.sourceCommand.load();

		// one tap stays with its source attack until accepted or cancelled
		const bool waitingForFollowUp =
			HasBufferedFollowUp(sourceCommand, action) &&
			g_currentAttackCommand.load() ==
			sourceCommand;

		if (now > expiresAt &&
			!waitingForFollowUp)
		{
			const std::uint64_t sequence =
				g_buffer.sequence.load();

			const ULONGLONG queuedAt =
				g_buffer.queuedAt.load();

			const ULONGLONG age =
				now >= queuedAt
				? now - queuedAt
				: 0;

			HJ::Logger::Info(
				std::format(
					"HJ BUFFER: expired #{} {} age={}ms",
					sequence,
					GetHJActionName(action),
					age
				)
			);

			ClearBufferedHJAction();

			return;
		}

		// pretty much type 1 transport on off ob off on off yaddayafda
		const bool pulseOn =
			!g_buffer.transportOn.load();

		g_buffer.transportOn.store(
			pulseOn
		);

		if (!pulseOn)
		{
			g_buffer.offeredAction.store(
				HJAction::None
			);

			return;
		}

		EmitHJAction(
			action,
			buttonMask,
			buttonValues
		);

		g_buffer.offeredAction.store(
			action
		);
		const auto sequence = g_buffer.sequence.load();
		if (g_transportTraceSequence.exchange(sequence) != sequence)
			TraceCadence("first-transport-on", static_cast<std::uint32_t>(action));
	}

	bool IsGameplayInputActive()
	{
		const ULONGLONG last =
			g_lastPlayerCombatUpdateAt.load();

		if (last == 0)
			return false;

		const ULONGLONG now =
			GetTickCount64();

		return now - last <=
			kGameplayInputFreshnessMs;
	}

	// attempting to add attack steering and commitment layers to lj

	float GetRH1SteeringAuthority(
		ULONGLONG elapsedMs)
	{
		constexpr ULONGLONG kFreeSteerUntilMs = 100;
		constexpr ULONGLONG kFullyCommittedAtMs = 320;

		if (elapsedMs <= kFreeSteerUntilMs)
			return 1.0f;

		if (elapsedMs >= kFullyCommittedAtMs)
			return 0.0f;

		const float t =
			static_cast<float>(
				elapsedMs - kFreeSteerUntilMs
				) /
			static_cast<float>(
				kFullyCommittedAtMs -
				kFreeSteerUntilMs
				);

		// smooth 1 -> 0
		const float smooth =
			t * t * (3.0f - 2.0f * t);

		return 1.0f - smooth;
	}

	bool HasLegFollowUp(
		std::uint32_t packedCommand,
		HJAction action)
	{
		return
			(action == HJAction::RightLeg ||
				action == HJAction::LeftLeg) &&
			HasBufferedFollowUp(packedCommand, action);
	}

	void SafeScaleStick(
		float* stickX,
		float* stickY,
		float authority) noexcept
	{
		if (!stickX || !stickY)
			return;

		__try
		{
			*stickX *= authority;
			*stickY *= authority;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	bool SafeApplyShoulderOwnership(
		std::uint32_t* buttonMask,
		std::uint8_t* buttonValues,
		bool physicalCircle) noexcept
	{
		constexpr std::uint32_t kLTMask = 0x10;
		constexpr std::uint32_t kRTMask = 0x20;
		constexpr std::uint32_t kLBMask = 0x40;
		constexpr std::uint32_t kRBMask = 0x80;

		__try
		{
			*buttonMask &= ~(
				kLTMask |
				kRTMask |
				kLBMask |
				kRBMask
				);

			buttonValues[4] = 0; // LT
			buttonValues[5] = 0; // RT
			buttonValues[6] = 0; // LB, HJ owns it now so vanilla guard never sees it
			buttonValues[7] = 0; // RB

			if (physicalCircle)
			{
				*buttonMask |= kLBMask;
				buttonValues[6] = 0xFF;
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void SafeSetLockOnMask(
		std::uint32_t* buttonMask,
		std::uint8_t* buttonValues) noexcept
	{
		constexpr std::uint32_t kRBMask = 0x80;

		__try
		{
			*buttonMask |= kRBMask;
			buttonValues[7] = 0xFF;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	std::uint64_t __fastcall ControllerTranslateHook(
		std::uint32_t controllerIndex,
		std::uint32_t* buttonMask,
		float* leftStickX,
		float* leftStickY,
		float* rightStickX,
		float* rightStickY,
		std::uint8_t* buttonValues)
	{
		const std::uint64_t result =
			g_controllerTranslateHook.call<std::uint64_t>(
				controllerIndex,
				buttonMask,
				leftStickX,
				leftStickY,
				rightStickX,
				rightStickY,
				buttonValues
			);

		if (controllerIndex == 0)
		{
			// acceptance can clear the buffer before native input finishes
			g_rightHandTransportOn.store(false);
		}

		if (controllerIndex != 0 ||
			buttonMask == nullptr ||
			buttonValues == nullptr)
		{
			return result;
		}

		if (!IsGameplayInputActive())
		{
			HJ::Hooks::GuardDirection::UpdateGuardButtons(0);

			g_previousShoulders = {};
			g_rtStableFrames = 0;
			g_ltStableFrames = 0;
			g_prevRawRT = false;
			g_prevRawLT = false;

			g_pendingTrigger =
				PendingTrigger::None;

			g_pendingTriggerStart = 0;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;

			ClearBufferedHJAction();

			return result;
		}

		bool fighting = false;

		if (!HJ::Engine::TryGetFightingState(fighting) ||
			!fighting)
		{
			g_previousShoulders = {};
			g_rtStableFrames = 0;
			g_ltStableFrames = 0;
			g_prevRawRT = false;
			g_prevRawLT = false;

			g_pendingTrigger =
				PendingTrigger::None;

			g_pendingTriggerStart = 0;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;

			ClearBufferedHJAction();
			HJ::Hooks::FighterCommand::
				ResetCurrentAttackTracking();

			return result;
		}

		XINPUT_STATE state{};

		if (XInputGetState(0, &state) != ERROR_SUCCESS)
		{
			HJ::Hooks::GuardDirection::UpdateGuardButtons(0);

			g_previousShoulders = {};
			g_rtStableFrames = 0;
			g_ltStableFrames = 0;
			g_prevRawRT = false;
			g_prevRawLT = false;

			g_pendingTrigger =
				PendingTrigger::None;

			g_pendingTriggerStart = 0;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;

			ClearBufferedHJAction();

			return result;
		}

		HJ::Hooks::GuardDirection::UpdateGuardButtons(state.Gamepad.wButtons);

		// RH1 steering prototype
		if (leftStickX != nullptr &&
			leftStickY != nullptr)
		{
			const std::uint32_t currentAttack =
				g_currentAttackCommand.load(
					std::memory_order_acquire
				);

			if (currentAttack == kHJRightHand1)
			{
				const ULONGLONG startedAt =
					g_currentAttackStartedAt.load(
						std::memory_order_acquire
					);

				if (startedAt != 0)
				{
					const ULONGLONG now =
						GetTickCount64();

					const ULONGLONG elapsed =
						now >= startedAt
						? now - startedAt
						: 0;

					const float authority =
						GetRH1SteeringAuthority(
							elapsed
						);

					SafeScaleStick(
						leftStickX,
						leftStickY,
						authority
					);
				}
			}
		}

		const bool physicalRB =
			(state.Gamepad.wButtons &
				XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;

		const bool physicalLB =
			(state.Gamepad.wButtons &
				XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;

		const bool rawRT =
			state.Gamepad.bRightTrigger >
			XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

		const bool rawLT =
			state.Gamepad.bLeftTrigger >
			XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

		// triggers as buttons need debouncing, a 1-frame flex blip
		// from an LB/RB slam must never become a kick
		if (rawRT)
			++g_rtStableFrames;
		else
			g_rtStableFrames = 0;

		if (rawLT)
			++g_ltStableFrames;
		else
			g_ltStableFrames = 0;

		const bool physicalRT =
			g_rtStableFrames >= kTriggerDebounceFrames;

		const bool physicalLT =
			g_ltStableFrames >= kTriggerDebounceFrames;

		if (rawRT && !g_prevRawRT && !physicalRT)
			HJ::Logger::Info("HJ TRIGGER: RT blip swallowed");

		if (rawLT && !g_prevRawLT && !physicalLT)
			HJ::Logger::Info("HJ TRIGGER: LT blip swallowed");

		g_prevRawRT = rawRT;
		g_prevRawLT = rawLT;

		// Circle is block, modifiers LB/RB pick side in GuardDirection hook
		const bool physicalCircle =
			(state.Gamepad.wButtons &
				XINPUT_GAMEPAD_B) != 0;

		const bool rbPressed =
			physicalRB &&
			!g_previousShoulders.rb;

		const bool lbPressed =
			physicalLB &&
			!g_previousShoulders.lb;

		const bool rtPressed =
			physicalRT &&
			!g_previousShoulders.rt;

		const bool ltPressed =
			physicalLT &&
			!g_previousShoulders.lt;

		if (rbPressed) TraceCadence("physical-RH");
		if (lbPressed) TraceCadence("physical-LH");
		if (rtPressed) TraceCadence("physical-RK");
		if (ltPressed) TraceCadence("physical-LK");

		// HJ owns the phys shoulder buttons
		if (!SafeApplyShoulderOwnership(
			buttonMask,
			buttonValues,
			physicalCircle))
		{
			return result;
		}

		// a newer hand press replaces any trigger still waiting for its chord
		if (rbPressed || lbPressed)
		{
			g_pendingTrigger = PendingTrigger::None;
			g_pendingTriggerStart = 0;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;
		}

		// hands.
		// LB/RB + Circle are block modifiers, not punches
		if (rbPressed && !physicalLB && !physicalCircle)
		{
			QueueHJAction(
				HJAction::RightHand
			);
		}

		if (lbPressed && !physicalRB && !physicalCircle)
		{
			QueueHJAction(
				HJAction::LeftHand
			);
		}

		// Legs / tackle.
		// same update is always tackle; root inputs still get the chord window
		const ULONGLONG now =
			GetTickCount64();

		if (g_pendingTrigger != PendingTrigger::None)
		{
			const HJAction pendingAction =
				g_pendingTrigger == PendingTrigger::RightLeg
				? HJAction::RightLeg
				: HJAction::LeftLeg;

			const bool alreadyQueued =
				g_pendingTriggerSequence != 0;

			const bool cancelled =
				g_pendingTriggerCancelSequence !=
				g_triggerCancelSequence.load();

			const bool sameQueuedKick =
				g_buffer.sequence.load() ==
				g_pendingTriggerSequence &&
				g_buffer.action.load() == pendingAction &&
				g_buffer.sourceCommand.load() ==
				g_currentAttackCommand.load();

			if (cancelled ||
				(alreadyQueued && !sameQueuedKick) ||
				now - g_pendingTriggerStart >=
				kTriggerChordWindowMs)
			{
				if (!cancelled && !alreadyQueued)
					QueueHJAction(pendingAction);

				g_pendingTrigger = PendingTrigger::None;
				g_pendingTriggerSequence = 0;
				g_pendingTriggerCancelSequence = 0;
			}
		}

		// Circle held = block, tackle chord loses
		if (rtPressed && ltPressed && !physicalCircle)
		{
			g_pendingTrigger = PendingTrigger::None;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;

			QueueHJAction(HJAction::Tackle);
		}
		// a second trigger can replace the kick only while it is still queued
		else if (!physicalCircle &&
			((g_pendingTrigger == PendingTrigger::RightLeg && ltPressed) ||
				(g_pendingTrigger == PendingTrigger::LeftLeg && rtPressed)))
		{
			g_pendingTrigger = PendingTrigger::None;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;

			QueueHJAction(HJAction::Tackle);
		}
		else if ((rtPressed || ltPressed) &&
			g_pendingTrigger == PendingTrigger::None)
		{
			const HJAction action =
				rtPressed ? HJAction::RightLeg : HJAction::LeftLeg;

			g_pendingTrigger =
				rtPressed ? PendingTrigger::RightLeg : PendingTrigger::LeftLeg;
			g_pendingTriggerStart = now;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = g_triggerCancelSequence.load();

			if (HasLegFollowUp(
				g_currentAttackCommand.load(), action))
			{
				QueueHJAction(action);
				g_pendingTriggerSequence = g_buffer.sequence.load();
			}
		}

		UpdateBufferedHJAction(
			buttonMask,
			buttonValues
		);

		// lock on just holds vanilla r1

		if (g_lockOnEnabled.load(
			std::memory_order_acquire))
		{
			SafeSetLockOnMask(
				buttonMask,
				buttonValues
			);
		}

		g_previousShoulders.rb = physicalRB;
		g_previousShoulders.lb = physicalLB;
		g_previousShoulders.rt = physicalRT;
		g_previousShoulders.lt = physicalLT;

		return result;
	}

	void UpdateControllerCombatInput()
	{
		static bool previousA = false;
		static bool previousR3 = false;
		static bool previousFighting = false;

		XINPUT_STATE state{};

		if (XInputGetState(0, &state) != ERROR_SUCCESS)
		{
			HJ::Hooks::GuardDirection::UpdateGuardButtons(0);

			previousA = false;
			previousR3 = false;

			return;
		}

		HJ::Hooks::GuardDirection::UpdateGuardButtons(state.Gamepad.wButtons);

		const bool currentA =
			(state.Gamepad.wButtons &
				XINPUT_GAMEPAD_A) != 0;

		const bool currentR3 =
			(state.Gamepad.wButtons &
				XINPUT_GAMEPAD_RIGHT_THUMB) != 0;

		bool fighting = false;

		const bool hasFightingState =
			HJ::Engine::TryGetFightingState(
				fighting
			);

		if (hasFightingState)
		{
			if (fighting && !previousFighting)
			{
				g_lockOnEnabled.store(false);
			}
			else if (!fighting && previousFighting)
			{
				g_lockOnEnabled.store(false);

				ClearBufferedHJAction();
				HJ::Hooks::FighterCommand::
					ResetCurrentAttackTracking();
			}

			previousFighting = fighting;
		}

		if (!hasFightingState || !fighting)
		{
			previousA = currentA;
			previousR3 = currentR3;

			return;
		}

		// r3 going to eventually be a toggle lock on

		if (currentR3 && !previousR3)
		{
			const bool enabled =
				!g_lockOnEnabled.load();

			g_lockOnEnabled.store(enabled);

			HJ::Logger::Info(
				enabled
				? "HJ LOCK ON: enabled"
				: "HJ LOCK ON: disabled"
			);
		}

		previousA = currentA;
		previousR3 = currentR3;
	}

	bool IsTraceActive()
	{
		const ULONGLONG until =
			g_traceUntil.load();

		if (until == 0)
			return false;

		const ULONGLONG now =
			GetTickCount64();

		if (now <= until)
			return true;

		ULONGLONG expected = until;

		if (g_traceUntil.compare_exchange_strong(
			expected,
			0))
		{
			HJ::Logger::Info(
				"=== COMBAT TRACE FINISHED ==="
			);
		}

		return false;
	}

	DWORD WINAPI TraceHotkeyThread(LPVOID)
	{
		HJ::Logger::Info(
			"Combat trace hotkey ready. Press F6 to capture."
		);

		while (true)
		{
			UpdateTraceHotkey();
			UpdateControllerCombatInput();

			UpdateEngineFightingDiagnostic();

			IsTraceActive();
			Sleep(10);
		}

		return 0;
	}

	bool SafeStrEquals(
		const char* str,
		const char* expected) noexcept
	{
		const auto addr =
			reinterpret_cast<std::uintptr_t>(str);

		if (addr < 0x10000 ||
			addr >= 0x00007FFFFFFF0000ULL ||
			expected == nullptr)
		{
			return false;
		}

		__try
		{
			for (std::size_t i = 0; i < 64; ++i)
			{
				const char c1 = str[i];
				const char c2 = expected[i];

				if (c1 != c2)
					return false;

				if (c1 == '\0')
					return true;
			}

			return false;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void SafeCopyCString(
		const char* str,
		char* out,
		std::size_t outSize) noexcept
	{
		if (out == nullptr || outSize == 0)
			return;

		out[0] = '\0';

		if (str == nullptr)
		{
			strncpy_s(out, outSize, "<null>", _TRUNCATE);
			return;
		}

		const auto addr =
			reinterpret_cast<std::uintptr_t>(str);

		if (addr < 0x10000 ||
			addr >= 0x00007FFFFFFF0000ULL)
		{
			strncpy_s(out, outSize, "<nonptr>", _TRUNCATE);
			return;
		}

		__try
		{
			std::size_t i = 0;
			for (; i + 1 < outSize; ++i)
			{
				const char c = str[i];
				out[i] = c;
				if (c == '\0')
					return;
			}
			out[i] = '\0';
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			strncpy_s(out, outSize, "<badptr>", _TRUNCATE);
		}
	}

	float SmootherStep01(float value) noexcept
	{
		value = std::clamp(value, 0.0f, 1.0f);
		return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
	}

	void ApplyEngineClockScales(
		std::uintptr_t base,
		float worldScale,
		float playerScale,
		float vfxScale) noexcept
	{
		if (base == 0)
			return;

		__try
		{
			auto* worldPtr = reinterpret_cast<float*>(base + kEngineWorldTimeScaleRva);
			auto* playerPtr = reinterpret_cast<float*>(base + kEnginePlayerTimeScaleRva);
			auto* vfxPtr = reinterpret_cast<float*>(base + kEngineVfxTimeScaleRva);

			// never overwrite a deeper native slow-mo (mortal reversal, ex boost, etc.)
			if (*worldPtr > worldScale)
				*worldPtr = worldScale;

			if (*playerPtr > playerScale)
				*playerPtr = playerScale;

			if (*vfxPtr > vfxScale)
				*vfxPtr = vfxScale;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	void ExtendInputBuffersForHitstop(ULONGLONG extraMs) noexcept
	{
		if (extraMs == 0)
			return;

		if (g_buffer.action.load(std::memory_order_acquire) != HJAction::None)
		{
			const ULONGLONG currentExpiry =
				g_buffer.expiresAt.load(std::memory_order_relaxed);

			if (currentExpiry != 0)
			{
				g_buffer.expiresAt.store(
					currentExpiry + extraMs,
					std::memory_order_release
				);
			}
		}

		if (g_pendingTrigger != PendingTrigger::None &&
			g_pendingTriggerStart != 0)
		{
			g_pendingTriggerStart += extraMs;
		}
	}

	enum class AttackHitZone : std::uint8_t
	{
		None,
		LightHead,
		GutBody,
		UppercutOrLauncher
	};

	struct BoxerAttackSpec
	{
		AttackHitZone zone = AttackHitZone::LightHead;
		float holdMs = 45.0f;
		float releaseMs = 60.0f;
		float intensity = 0.60f;
		float dirX = 1.0f;
	};

	struct EnemyStaggerSlot
	{
		std::uintptr_t container = 0;
		ULONGLONG lastHitAt = 0;
		ULONGLONG gutStaggerUntil = 0;
		std::uint32_t comboCount = 0;
	};

	constexpr std::size_t kEnemyStaggerSlotCount = 16;
	EnemyStaggerSlot g_enemyStaggerSlots[kEnemyStaggerSlotCount]{};

	EnemyStaggerSlot& AcquireEnemyStaggerSlot(
		std::uintptr_t stateContainer,
		ULONGLONG now) noexcept
	{
		std::size_t oldestIndex = 0;
		ULONGLONG oldestTick = ~0ULL;

		for (std::size_t i = 0; i < kEnemyStaggerSlotCount; ++i)
		{
			if (g_enemyStaggerSlots[i].container == stateContainer)
				return g_enemyStaggerSlots[i];

			if (g_enemyStaggerSlots[i].container == 0)
			{
				oldestIndex = i;
				break;
			}

			if (g_enemyStaggerSlots[i].lastHitAt < oldestTick)
			{
				oldestTick = g_enemyStaggerSlots[i].lastHitAt;
				oldestIndex = i;
			}
		}

		EnemyStaggerSlot& slot = g_enemyStaggerSlots[oldestIndex];
		slot.container = stateContainer;
		slot.lastHitAt = now;
		slot.gutStaggerUntil = 0;
		slot.comboCount = 0;
		return slot;
	}

	bool IsEnemyGutStaggered(
		std::uintptr_t stateContainer,
		ULONGLONG now,
		std::uint32_t& outComboCount) noexcept
	{
		outComboCount = 0;
		for (std::size_t i = 0; i < kEnemyStaggerSlotCount; ++i)
		{
			const EnemyStaggerSlot& slot = g_enemyStaggerSlots[i];
			if (slot.container == stateContainer)
			{
				if (now >= slot.lastHitAt && (now - slot.lastHitAt) <= 1350)
					outComboCount = slot.comboCount;
				return slot.gutStaggerUntil != 0 && now <= slot.gutStaggerUntil;
			}
		}
		return false;
	}

	bool GetBoxerAttackSpecs(
		std::uint32_t packedCommand,
		BoxerAttackSpec& outSpec) noexcept
	{
		if ((packedCommand & 0xFFFF) != 0x73)
			return false;

		const std::uint32_t variant = packedCommand >> 16;
		if (variant >= 0x16 && variant <= 0x19)
			return false; // Tackle moves use dedicated camera sequencing

		switch (variant)
		{
			// Jabs & lead straights (quick head snap)
		case 0x01: case 0x14:
			outSpec = { AttackHitZone::LightHead, 34.0f, 48.0f, 0.45f, 1.0f };
			return true;
		case 0x05: case 0x26:
			outSpec = { AttackHitZone::LightHead, 34.0f, 48.0f, 0.45f, -1.0f };
			return true;

			// Body hooks & gut blows (midsection fold)
		case 0x02: case 0x2C:
			outSpec = { AttackHitZone::GutBody, 60.0f, 78.0f, 0.78f, 1.0f };
			return true;
		case 0x06: case 0x0D: case 0x2D: case 0x32: case 0x35:
			outSpec = { AttackHitZone::GutBody, 60.0f, 78.0f, 0.78f, -1.0f };
			return true;

			// Heavy uppercuts & string finishers
		case 0x03: case 0x04: case 0x15: case 0x1E: case 0x23:
		case 0x29: case 0x34: case 0x37: case 0x38: case 0x3C:
			outSpec = { AttackHitZone::UppercutOrLauncher, 88.0f, 112.0f, 1.15f, 1.0f };
			return true;
		case 0x07: case 0x08: case 0x12: case 0x1B: case 0x2A:
		case 0x31: case 0x3A: case 0x3B:
			outSpec = { AttackHitZone::UppercutOrLauncher, 88.0f, 112.0f, 1.15f, -1.0f };
			return true;

			// Mid kicks & knees (stomach drive)
		case 0x09: case 0x11: case 0x1F: case 0x27: case 0x2E:
			outSpec = { AttackHitZone::GutBody, 70.0f, 90.0f, 0.95f, 1.0f };
			return true;
		case 0x0C: case 0x10: case 0x28: case 0x2F:
			outSpec = { AttackHitZone::GutBody, 70.0f, 90.0f, 0.95f, -1.0f };
			return true;

			// High / lifting kicks & knees
		case 0x0A: case 0x0B: case 0x0E: case 0x1A: case 0x25: case 0x33: case 0x39:
			outSpec = { AttackHitZone::UppercutOrLauncher, 70.0f, 90.0f, 0.95f, 1.0f };
			return true;
		case 0x0F: case 0x13: case 0x1C: case 0x1D: case 0x20:
		case 0x22: case 0x2B: case 0x36: case 0x3D:
			outSpec = { AttackHitZone::UppercutOrLauncher, 70.0f, 90.0f, 0.95f, -1.0f };
			return true;

		default:
			outSpec = { AttackHitZone::LightHead, 45.0f, 60.0f, 0.60f, 1.0f };
			return true;
		}
	}

	void ApplyHitPacketReactionOverrides(
		std::uintptr_t hitPacket,
		std::uint32_t targetDamageLevel,
		bool forceKnockdown,
		std::uint8_t hitHeightZone = 0,
		std::uint8_t hitDirection = 0) noexcept
	{
		if (hitPacket == 0)
			return;

		__try
		{
			auto* levelPtr =
				reinterpret_cast<std::uint32_t*>(hitPacket + 0x80);
			auto* heightPtr =
				reinterpret_cast<std::uint8_t*>(hitPacket + 0x60);
			auto* dirPtr =
				reinterpret_cast<std::uint8_t*>(hitPacket + 0x61);

			if (hitHeightZone != 0)
			{
				*heightPtr = hitHeightZone;
			}
			*dirPtr = hitDirection;

			if (forceKnockdown)
			{
				// Clean grounded knockdown (lvl4): enemy drops right to the floor
				*heightPtr = 1;
				*levelPtr = 4u;
			}
			else if (targetDamageLevel > 0)
			{
				*levelPtr = targetDamageLevel;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	void ApplyDamageStateHitZoneOverride(
		std::uintptr_t stateObject,
		std::uint8_t hitZoneByte,
		std::uint32_t standingLevelOverride) noexcept
	{
		if (stateObject == 0)
			return;

		__try
		{
			// +0x430 is the attack collision hit height (1 = head/high, 2 = gut/body)
			if (hitZoneByte != 0)
			{
				*reinterpret_cast<std::uint8_t*>(stateObject + 0x430) =
					hitZoneByte;
			}

			// +0x110 is hitPacket+0x80 inside the state object; 0 means knockdown/launch,
			// 1..4 are standing stagger severities
			auto* embeddedLevel =
				reinterpret_cast<std::uint32_t*>(stateObject + 0x110);
			if (*embeddedLevel != 0 && standingLevelOverride != 0)
			{
				*embeddedLevel = standingLevelOverride;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	bool IsDamageStateObjectKnockdown(const void* stateObject) noexcept
	{
		if (stateObject == nullptr)
			return false;

		__try
		{
			const auto addr = reinterpret_cast<std::uintptr_t>(stateObject);
			return *reinterpret_cast<const std::uint32_t*>(addr + 0x110) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	EnemyReactionKind ClassifyEnemyReactionState(
		const char* stateName,
		const void* stateObject) noexcept
	{
		if (stateName == nullptr)
			return EnemyReactionKind::None;

		if (SafeStrEquals(stateName, "HumanEnemyDamage") ||
			SafeStrEquals(stateName, "HumanDamage") ||
			SafeStrEquals(stateName, "TigerDamage") ||
			SafeStrEquals(stateName, "SimpleDamage"))
		{
			if (IsDamageStateObjectKnockdown(stateObject))
				return EnemyReactionKind::HeavyKnockdown;

			return EnemyReactionKind::StandardHit;
		}

		if (SafeStrEquals(stateName, "TigerFlyDamage") ||
			SafeStrEquals(stateName, "FlyDamageF") ||
			SafeStrEquals(stateName, "FlyDamageB") ||
			SafeStrEquals(stateName, "FlyDamageL") ||
			SafeStrEquals(stateName, "FlyDamageR") ||
			SafeStrEquals(stateName, "RagdollShotDamage") ||
			SafeStrEquals(stateName, "RagdollInstant") ||
			SafeStrEquals(stateName, "RagdollDown") ||
			SafeStrEquals(stateName, "RagdollDead") ||
			SafeStrEquals(stateName, "SimpleDead") ||
			SafeStrEquals(stateName, "GuardBreak") ||
			SafeStrEquals(stateName, "GuardBreakDamage") ||
			SafeStrEquals(stateName, "JustGuardBreakDamage"))
		{
			return EnemyReactionKind::HeavyKnockdown;
		}

		if (SafeStrEquals(stateName, "GuardReaction") ||
			SafeStrEquals(stateName, "FriendlyGuard"))
		{
			return EnemyReactionKind::BlockedHit;
		}

		return EnemyReactionKind::None;
	}

	void CommitHitstopImpulse(
		float holdMs,
		float releaseMs,
		float intensity,
		float dirX,
		bool isPlayerBlock) noexcept
	{
		const ULONGLONG now = GetTickCount64();
		const ULONGLONG prevStart = g_hitstop.startedAt;
		const float prevHold = g_hitstop.holdMs;
		const float prevRelease = g_hitstop.releaseMs;
		const float prevTotal = prevHold + prevRelease;

		const bool active =
			prevStart != 0 &&
			now >= prevStart &&
			static_cast<float>(now - prevStart) < prevTotal;

		// Multi-target cleave damping: slightly extend the active hold if another hit registers within 55ms
		if (active && (now - prevStart) <= 55)
		{
			const float extendedHold = std::min(prevHold + 14.0f, 115.0f);
			const float addedMs = std::max(0.0f, extendedHold - prevHold);

			g_hitstop.holdMs = extendedHold;
			g_hitstop.releaseMs = std::max(prevRelease, releaseMs);
			g_hitstop.intensity = std::max(g_hitstop.intensity, intensity);

			ExtendInputBuffersForHitstop(static_cast<ULONGLONG>(addedMs + 0.5f));
			HJ::Hooks::FighterCommand::TickHitstopFrameScale();
			return;
		}

		g_hitstop.holdMs = holdMs;
		g_hitstop.releaseMs = releaseMs;
		g_hitstop.intensity = intensity;
		g_hitstop.dirX = dirX;
		g_hitstop.playerBlock = isPlayerBlock;
		g_hitstop.startedAt = now;
		g_hitstop.sequence.fetch_add(1, std::memory_order_release);

		ExtendInputBuffersForHitstop(static_cast<ULONGLONG>(holdMs + releaseMs * 0.5f));
		HJ::Hooks::FighterCommand::TickHitstopFrameScale();
	}

	void TryShapeEnemyHitPacket(
		std::uintptr_t stateContainer,
		std::uintptr_t hitPacket) noexcept
	{
		if (stateContainer == 0 || hitPacket == 0)
			return;

		if (g_tackleActive.load(std::memory_order_acquire))
			return;

		const std::uintptr_t playerObject = HJ_GetPlayerCombatObject();
		if (playerObject == 0 || stateContainer == playerObject)
			return;

		const std::uint32_t currentAttack =
			g_currentAttackCommand.load(std::memory_order_acquire);
		const ULONGLONG attackStartedAt =
			g_currentAttackStartedAt.load(std::memory_order_acquire);
		if (currentAttack == 0 || attackStartedAt == 0)
			return;

		const ULONGLONG now = GetTickCount64();
		if (now < attackStartedAt || (now - attackStartedAt) > 900)
			return;

		BoxerAttackSpec spec{};
		if (!GetBoxerAttackSpecs(currentAttack, spec))
			return;

		std::uint32_t comboCount = 0;
		const bool gutStaggered =
			IsEnemyGutStaggered(stateContainer, now, comboCount);

		EnemyStaggerSlot& slot =
			AcquireEnemyStaggerSlot(stateContainer, now);
		const bool chained =
			(now >= slot.lastHitAt && (now - slot.lastHitAt) <= 1350);
		slot.comboCount = chained ? (slot.comboCount + 1) : 1;
		slot.lastHitAt = now;

		if (spec.zone == AttackHitZone::LightHead)
		{
			// Light head/straight: crisp head flinch
			ApplyHitPacketReactionOverrides(hitPacket, 0, false, 1, 0);
		}
		else if (spec.zone == AttackHitZone::GutBody)
		{
			// gut/body blow flds enemy forward clutching stomach; 1.25s follow-up window
			slot.gutStaggerUntil = now + 1250;
			ApplyHitPacketReactionOverrides(hitPacket, 0, false, 2, 0);
		}
		else if (spec.zone == AttackHitZone::UppercutOrLauncher)
		{
			// Uppercut forces grounded knockdown if enemy is gutstaggered
			if (gutStaggered)
			{
				slot.gutStaggerUntil = 0;
				slot.comboCount = 0;
				ApplyHitPacketReactionOverrides(hitPacket, 0, true, 1, 0);
			}
			else
			{
				ApplyHitPacketReactionOverrides(hitPacket, 0, false, 1, 0);
			}
		}
	}

	bool __fastcall ProcessFighterHitDamageHook(
		std::uintptr_t stateContainer,
		std::uintptr_t hitPacket)
	{
		TryShapeEnemyHitPacket(stateContainer, hitPacket);

		return g_processHitDamageHook.call<bool>(
			stateContainer,
			hitPacket
		);
	}

	bool __fastcall EnemyDamageDispatchHook(
		std::uintptr_t stateContainer,
		std::uintptr_t hitPacket)
	{
		TryShapeEnemyHitPacket(stateContainer, hitPacket);

		return g_enemyDamageDispatchHook.call<bool>(
			stateContainer,
			hitPacket
		);
	}

	void* __fastcall DamageStateInitHook(
		std::uintptr_t stateObject,
		std::uintptr_t stateContainer,
		std::uintptr_t hitPacket)
	{
		void* result = g_damageStateInitHook.call<void*>(
			stateObject,
			stateContainer,
			hitPacket
		);

		const std::uintptr_t playerObject = HJ_GetPlayerCombatObject();
		if (playerObject != 0 &&
			stateContainer != 0 &&
			stateContainer != playerObject &&
			!g_tackleActive.load(std::memory_order_acquire))
		{
			const std::uint32_t currentAttack =
				g_currentAttackCommand.load(std::memory_order_acquire);
			const ULONGLONG attackStartedAt =
				g_currentAttackStartedAt.load(std::memory_order_acquire);
			const ULONGLONG now = GetTickCount64();

			if (currentAttack != 0 &&
				attackStartedAt != 0 &&
				now >= attackStartedAt &&
				(now - attackStartedAt) <= 900)
			{
				BoxerAttackSpec spec{};
				if (GetBoxerAttackSpecs(currentAttack, spec))
				{
					EnemyStaggerSlot& slot =
						AcquireEnemyStaggerSlot(stateContainer, now);
					const bool chained =
						(now >= slot.lastHitAt && (now - slot.lastHitAt) <= 1350);
					slot.comboCount = chained ? (slot.comboCount + 1) : 1;
					slot.lastHitAt = now;

					if (IsDamageStateObjectKnockdown(
						reinterpret_cast<const void*>(stateObject)))
					{
						slot.gutStaggerUntil = 0;
						slot.comboCount = 0;
					}
					else if (spec.zone == AttackHitZone::GutBody)
					{
						// Gut/body blow folds the enemy forward; 1.25s follow-up window
						slot.gutStaggerUntil = now + 1250;
						ApplyDamageStateHitZoneOverride(stateObject, 2, 0);
					}
					else if (spec.zone == AttackHitZone::LightHead ||
						spec.zone == AttackHitZone::UppercutOrLauncher)
					{
						ApplyDamageStateHitZoneOverride(stateObject, 1, 0);
					}
				}
			}
		}

		return result;
	}

	void TryTriggerEnemyHitstop(
		std::uintptr_t stateContainer,
		const void* stateObject,
		const char* stateName) noexcept
	{
		if (stateContainer == 0 || stateName == nullptr)
			return;

		if (g_tackleActive.load(std::memory_order_acquire))
			return;

		const std::uintptr_t playerObject = HJ_GetPlayerCombatObject();
		if (playerObject == 0 || stateContainer == playerObject)
			return;

		const std::uint32_t currentAttack =
			g_currentAttackCommand.load(std::memory_order_acquire);
		const ULONGLONG attackStartedAt =
			g_currentAttackStartedAt.load(std::memory_order_acquire);

		if (currentAttack == 0 || attackStartedAt == 0)
			return;

		const ULONGLONG now = GetTickCount64();
		if (now < attackStartedAt || (now - attackStartedAt) > 900)
			return;

		const EnemyReactionKind reaction =
			ClassifyEnemyReactionState(stateName, stateObject);
		if (reaction == EnemyReactionKind::None)
			return;

		if (reaction == EnemyReactionKind::HeavyKnockdown)
		{
			EnemyStaggerSlot& slot =
				AcquireEnemyStaggerSlot(stateContainer, now);
			slot.gutStaggerUntil = 0;
			slot.comboCount = 0;
		}

		BoxerAttackSpec spec{};
		if (!GetBoxerAttackSpecs(currentAttack, spec))
			return;

		if (reaction == EnemyReactionKind::HeavyKnockdown)
		{
			spec.holdMs += 22.0f;
			spec.releaseMs += 32.0f;
			spec.intensity = std::min(spec.intensity * 1.25f, 1.40f);
		}
		else if (reaction == EnemyReactionKind::BlockedHit)
		{
			spec.holdMs = std::clamp(spec.holdMs * 0.58f, 22.0f, 36.0f);
			spec.releaseMs = 38.0f;
			spec.intensity = std::max(spec.intensity * 0.65f, 0.42f);
		}

		CommitHitstopImpulse(
			spec.holdMs,
			spec.releaseMs,
			spec.intensity,
			spec.dirX,
			false
		);
	}

	void* __fastcall NamedStateHook(
		std::uintptr_t stateContainer,
		void* stateObject,
		const char* stateName)
	{
		const auto caller =
			reinterpret_cast<std::uintptr_t>(
				_ReturnAddress()
				);

		const bool isWaitKamae =
			SafeStrEquals(stateName, "WaitKamae");

		if (isWaitKamae &&
			stateContainer == g_currentAttackContainer.load())
		{
			TraceCadence("WaitKamae-request");
			const std::uint32_t endingCommand = g_currentAttackCommand.exchange(0);
			HJ::Hooks::FighterCommand::ResetCurrentAttackTracking();

			if (endingCommand != 0)
			{
				const std::uint32_t sourceCommand = g_buffer.sourceCommand.load();
				const HJAction currentBufferAction = g_buffer.action.load();

				// If the buffer is still holding an action (like LH), don't cancel it!
				if (sourceCommand == endingCommand && currentBufferAction == HJAction::None)
				{
					HJ::Hooks::FighterCommand::CancelBufferedAction();
				}
			}
		}

		if (stateName != nullptr)
		{
			// Identity is the per-tick player combat object, never the
			// attack container: attack tracking resets to 0 mid-stun, and
			// the old loose rule let any container's WaitKamae clear the
			// latch instantly (live bug 2026-10-15: set on 0x...02F0,
			// cleared by 0x...6AEE0/0x...189D0). Non-zero + exact match.
			const std::uintptr_t playerObject = HJ_GetPlayerCombatObject();

			if (playerObject != 0 &&
				stateContainer == playerObject)
			{
				if (SafeStrEquals(stateName, "StunMode") ||
					SafeStrEquals(stateName, "PlayerRagdollMode"))
				{
					char safeName[64]{};
					SafeCopyCString(stateName, safeName, sizeof(safeName));

					g_playerStunned.store(true, std::memory_order_release);
					g_playerStunnedAt.store(GetTickCount64(), std::memory_order_release);
					HJ::Logger::Info(
						std::format(
							"HJ EVADE STUN LATCH: set by {} container=0x{:X}",
							safeName,
							stateContainer
						)
					);
				}
				else if (isWaitKamae ||
					SafeStrEquals(stateName, "PlayerSway") ||
					SafeStrEquals(stateName, "Guard"))
				{
					if (g_playerStunned.exchange(false, std::memory_order_acq_rel))
					{
						char safeName[64]{};
						SafeCopyCString(stateName, safeName, sizeof(safeName));

						HJ::Logger::Info(
							std::format(
								"HJ EVADE STUN LATCH: cleared by {} container=0x{:X}",
								safeName,
								stateContainer
							)
						);
					}
				}
			}
		}

		if (IsTraceActive())
		{
			char safeName[64]{};
			SafeCopyCString(stateName, safeName, sizeof(safeName));

			std::ostringstream stream;

			stream
				<< "STATE TRACE +" << std::dec
				<< GetTickCount64() - g_traceStartedAt.load() << "ms"
				<< " name=\""
				<< safeName
				<< "\""
				<< " container=0x"
				<< std::hex
				<< stateContainer
				<< " object=0x"
				<< reinterpret_cast<std::uintptr_t>(
					stateObject
					)
				<< " caller=0x"
				<< caller;

			HJ::Hooks::ActionRequest::TraceAttackObject(
				stateContainer,
				stateObject,
				stateName,
				caller
			);

			HJ::Logger::Info(
				stream.str()
			);
		}

		void* result = g_namedStateHook.call<void*>(
			stateContainer,
			stateObject,
			stateName
		);

		if (result != nullptr &&
			result == stateObject)
		{
			if (SafeStrEquals(stateName, "PlayerSway"))
			{
				HJ::Hooks::ActionRequest::
					NotifyEvadeAccepted(stateContainer);
			}
			else
			{
				TryTriggerEnemyHitstop(stateContainer, stateObject, stateName);
			}
		}

		return result;
	}

	std::uint64_t __fastcall CheckEligibilityHook(
		std::uint64_t context,
		std::uint32_t key,
		std::uint32_t mode,
		std::uint8_t option)
	{
		const bool traceActive =
			IsTraceActive();

		const auto caller =
			reinterpret_cast<std::uintptr_t>(
				_ReturnAddress()
				);

		// run the og de function first so it can record its eligibility result
		const std::uint64_t result =
			g_checkEligibilityHook.call<std::uint64_t>(
				context,
				key,
				mode,
				option
			);

		const bool eligible =
			(result & 0xFF) != 0;

		// function casn execute OOOFFFTEEENNNN only log when something about the check changes on this thread

		if (traceActive)
		{
			const TraceEntry entry{
				key,
				mode,
				option,
				caller,
				eligible,
				g_attackTraceSerial.load()
			};

			bool firstOccurrence = false;

			{
				std::scoped_lock lock(g_traceMutex);

				firstOccurrence =
					g_traceEntries.insert(entry).second;
			}

			if (firstOccurrence)
			{
				const ULONGLONG now =
					GetTickCount64();

				const ULONGLONG elapsed =
					now - g_traceStartedAt.load();

				std::ostringstream stream;

				stream
					<< "TRACE +"
					<< std::dec
					<< elapsed
					<< "ms"
					<< " thread="
					<< GetCurrentThreadId()
					<< " context=0x"
					<< std::hex
					<< context
					<< " key=0x"
					<< key
					<< " mode=0x"
					<< mode
					<< " option=0x"
					<< static_cast<unsigned int>(option)
					<< " caller=0x"
					<< caller
					<< " result="
					<< (eligible ? "true" : "false");

				HJ::Logger::Info(
					stream.str()
				);
			}
		}

		return result;
	}
}

namespace HJ::Hooks::FighterCommand
{
	bool IsCombatTraceActive()
	{
		return IsTraceActive();
	}

	bool IsPlayerStunLatched()
	{
		if (!g_playerStunned.load(
			std::memory_order_acquire
		))
		{
			return false;
		}

		const ULONGLONG age =
			GetTickCount64() - g_playerStunnedAt.load(
				std::memory_order_acquire
			);

		if (age > kStunLatchExpiryMs)
		{
			g_playerStunned.store(
				false,
				std::memory_order_release
			);

			return false;
		}

		return true;
	}

	void CancelBufferedAction()
	{
		// the first trigger may still be waiting outside the action buffer
		g_triggerCancelSequence.fetch_add(1);

		const HJAction action =
			g_buffer.action.load();

		if (action == HJAction::None)
			return;

		const std::uint64_t sequence =
			g_buffer.sequence.load();

		HJ::Logger::Info(
			std::format(
				"HJ BUFFER: cancelled #{} {}",
				sequence,
				GetHJActionName(action)
			)
		);

		ClearBufferedHJAction();
	}

	extern "C"
		__declspec(dllexport)
		std::uint64_t HJ_GetTackleSequence()
	{
		return g_tackleSequence.load(
			std::memory_order_acquire
		);
	}

	extern "C"
		__declspec(dllexport)
		std::uint64_t HJ_GetTackleStartTick()
	{
		return g_tackleStartedAt.load(
			std::memory_order_acquire
		);
	}

	extern "C"
		__declspec(dllexport)
		bool HJ_IsTackleActive()
	{
		return g_tackleActive.load(
			std::memory_order_acquire
		);
	}

	extern "C"
		__declspec(dllexport)
		bool HJ_GetHitImpactState(
			std::uint64_t* outSequence,
			std::uint64_t* outStartTick,
			float* outHoldMs,
			float* outReleaseMs,
			float* outIntensity,
			float* outDirX)
	{
		if (!outSequence || !outStartTick ||
			!outHoldMs || !outReleaseMs ||
			!outIntensity || !outDirX)
		{
			return false;
		}

		const std::uint64_t seq =
			g_hitstop.sequence.load(std::memory_order_acquire);
		const ULONGLONG startedAt = g_hitstop.startedAt;

		if (seq == 0 || startedAt == 0)
			return false;

		*outSequence = seq;
		*outStartTick = startedAt;
		*outHoldMs = g_hitstop.holdMs;
		*outReleaseMs = g_hitstop.releaseMs;
		*outIntensity = g_hitstop.intensity;
		*outDirX = g_hitstop.dirX;
		return true;
	}

	void NotifyPlayerBlockedHit() noexcept
	{
		if (g_tackleActive.load(std::memory_order_acquire))
			return;

		const int side = HJ::Hooks::GuardDirection::CurrentSideId();
		const float dirX =
			side == 1 ? -1.0f :
			side == 2 ? 1.0f : 0.0f;

		CommitHitstopImpulse(26.0f, 40.0f, 0.60f, dirX, true);
	}

	void TickHitstopFrameScale() noexcept
	{
		const ULONGLONG startedAt = g_hitstop.startedAt;
		if (startedAt == 0)
			return;

		if (g_tackleActive.load(std::memory_order_acquire))
		{
			g_hitstop.startedAt = 0;
			return;
		}

		const ULONGLONG now = GetTickCount64();
		if (now < startedAt)
			return;

		const float holdMs = g_hitstop.holdMs;
		const float releaseMs = g_hitstop.releaseMs;
		const float intensity = g_hitstop.intensity;
		const float totalMs = holdMs + releaseMs;
		const float elapsed = static_cast<float>(now - startedAt);

		if (elapsed >= totalMs || totalMs <= 0.0f)
		{
			g_hitstop.startedAt = 0;
			return;
		}

		const auto base =
			reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (base == 0)
			return;

		const bool isPlayerBlock = g_hitstop.playerBlock;

		// Normalized impact intensity weighting
		const float weight01 =
			std::clamp((intensity - 0.40f) / 0.85f, 0.0f, 1.0f);
		const float troughFloor =
			0.18f - 0.10f * weight01;
		const float vfxFloor =
			0.38f - 0.10f * weight01;
		const float easeInMs =
			std::clamp(holdMs * 0.25f, 12.0f, 24.0f);

		float worldScale = 1.0f;
		float playerScale = 1.0f;
		float vfxScale = 1.0f;

		if (elapsed < easeInMs && easeInMs > 0.0f)
		{
			const float inT = SmootherStep01(elapsed / easeInMs);
			worldScale = 1.0f - (1.0f - troughFloor) * inT;
			playerScale = worldScale;
			vfxScale = 1.0f - (1.0f - vfxFloor) * inT;
		}
		else if (elapsed < holdMs)
		{
			const float holdSpan = std::max(1.0f, holdMs - easeInMs);
			const float holdT = SmootherStep01((elapsed - easeInMs) / holdSpan);

			const float worldEnd = troughFloor + 0.020f;
			const float playerEnd =
				isPlayerBlock ? worldEnd : (troughFloor + 0.035f);

			worldScale = troughFloor + (worldEnd - troughFloor) * holdT;
			playerScale = troughFloor + (playerEnd - troughFloor) * holdT;
			vfxScale = vfxFloor + 0.05f * holdT;
		}
		else if (releaseMs > 0.0f)
		{
			const float u =
				std::clamp((elapsed - holdMs) / releaseMs, 0.0f, 1.0f);
			const float easeOut = SmootherStep01(u);

			const float worldStart = troughFloor + 0.020f;
			const float playerStart =
				isPlayerBlock ? worldStart : (troughFloor + 0.035f);
			const float vfxStart = vfxFloor + 0.05f;

			worldScale = worldStart + (1.0f - worldStart) * easeOut;
			playerScale = playerStart + (1.0f - playerStart) * easeOut;
			vfxScale = vfxStart + (1.0f - vfxStart) * easeOut;
		}

		ApplyEngineClockScales(base, worldScale, playerScale, vfxScale);
	}

	void ResetCurrentAttackTracking()
	{
		g_currentAttackCommand.store(0);
		g_currentAttackContainer.store(0);

		g_currentAttackStartedAt.store(
			0,
			std::memory_order_release
		);

		g_tackleActive.store(
			false,
			std::memory_order_release
		);
	}

	void NotifyPlayerCombatUpdate()
	{
		g_lastPlayerCombatUpdateAt.store(
			GetTickCount64()
		);

		TickHitstopFrameScale();
		HJ::Hooks::GuardMotion::Tick();
	}

	bool Initialize()
	{
		const HMODULE gameModule =
			GetModuleHandleA(nullptr);

		if (!gameModule)
		{
			Logger::Error(
				"FighterCommand hook: failed to get game module."
			);

			return false;
		}

		const auto base =
			reinterpret_cast<std::uintptr_t>(
				gameModule
				);

		// xinput_pollandtranslatecontroller

		constexpr std::uintptr_t ControllerTranslateRva =
			0x019F2EC0;

		constexpr std::uintptr_t CheckEligibilityRva =
			0x02E8F500;

		// FUN_1403B43D0

		constexpr std::uintptr_t NamedStateRva =
			0x003B43D0;

		constexpr std::uintptr_t RuntimeBitConditionRva =
			0x02C1B930;

		const auto target =
			base + CheckEligibilityRva;

		const auto namedStateTarget =
			base + NamedStateRva;

		const auto controllerTranslateTarget =
			base + ControllerTranslateRva;

		{
			std::ostringstream stream;

			stream
				<< "Installing FighterCommand eligibility hook at 0x"
				<< std::hex
				<< target;

			Logger::Info(
				stream.str()
			);
		}

		g_checkEligibilityHook =
			safetyhook::create_inline(
				reinterpret_cast<void*>(target),
				reinterpret_cast<void*>(
					&CheckEligibilityHook
					)
			);

		if (!g_checkEligibilityHook)
		{
			Logger::Error(
				"Failed to install FighterCommand eligibility hook."
			);

			return false;
		}

		Logger::Info(
			"FighterCommand eligibility hook installed."
		);

		{
			std::ostringstream stream;

			stream
				<< "Installing named state hook at 0x"
				<< std::hex
				<< namedStateTarget;

			Logger::Info(
				stream.str()
			);
		}

		g_namedStateHook =
			safetyhook::create_inline(
				reinterpret_cast<void*>(
					namedStateTarget
					),
				reinterpret_cast<void*>(
					&NamedStateHook
					)
			);

		if (!g_namedStateHook)
		{
			Logger::Error(
				"Failed to install named state hook."
			);

			return false;
		}

		Logger::Info(
			"Named state hook installed."
		);

		{
			std::ostringstream stream;

			stream
				<< "Installing controller translate hook at 0x"
				<< std::hex
				<< controllerTranslateTarget;

			Logger::Info(
				stream.str()
			);
		}
		g_controllerTranslateHook =
			safetyhook::create_inline(
				reinterpret_cast<void*>(
					controllerTranslateTarget
					),
				reinterpret_cast<void*>(
					&ControllerTranslateHook
					)
			);

		if (!g_controllerTranslateHook)
		{
			Logger::Error(
				"Failed to install controller translate hook."
			);

			return false;
		}

		Logger::Info(
			"Controller translate hook installed."
		);

		constexpr std::uintptr_t ProcessHitDamageRva = 0x02EE4CA0;
		constexpr std::uint8_t kExpectedProcessHitBytes[] = {
			0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x70, 0x20,
			0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x05, 0x00, 0x00
		};

		const auto processHitTarget = base + ProcessHitDamageRva;
		if (std::memcmp(
			reinterpret_cast<const void*>(processHitTarget),
			kExpectedProcessHitBytes,
			sizeof(kExpectedProcessHitBytes)) == 0)
		{
			g_processHitDamageHook = safetyhook::create_inline(
				reinterpret_cast<void*>(processHitTarget),
				reinterpret_cast<void*>(&ProcessFighterHitDamageHook)
			);
			if (g_processHitDamageHook)
				Logger::Info("Fighter hit damage hook installed.");
		}

		constexpr std::uintptr_t DamageStateInitRva = 0x02EE2B10;
		constexpr std::uint8_t kExpectedDamageInitBytes[] = {
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08,
			0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
		};

		const auto damageInitTarget = base + DamageStateInitRva;
		if (std::memcmp(
			reinterpret_cast<const void*>(damageInitTarget),
			kExpectedDamageInitBytes,
			sizeof(kExpectedDamageInitBytes)) == 0)
		{
			g_damageStateInitHook = safetyhook::create_inline(
				reinterpret_cast<void*>(damageInitTarget),
				reinterpret_cast<void*>(&DamageStateInitHook)
			);
			if (g_damageStateInitHook)
				Logger::Info("Damage state init hook installed.");
		}

		constexpr std::uintptr_t EnemyDamageDispatchRva = 0x02F20220;
		constexpr std::uint8_t kExpectedEnemyDispatchBytes[] = {
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
			0x57, 0x48, 0x81, 0xEC, 0x80, 0x03, 0x00, 0x00
		};

		const auto enemyDispatchTarget = base + EnemyDamageDispatchRva;
		if (std::memcmp(
			reinterpret_cast<const void*>(enemyDispatchTarget),
			kExpectedEnemyDispatchBytes,
			sizeof(kExpectedEnemyDispatchBytes)) == 0)
		{
			g_enemyDamageDispatchHook = safetyhook::create_inline(
				reinterpret_cast<void*>(enemyDispatchTarget),
				reinterpret_cast<void*>(&EnemyDamageDispatchHook)
			);
			if (g_enemyDamageDispatchHook)
				Logger::Info("Enemy damage dispatch hook installed.");
		}

		HANDLE traceThread =
			CreateThread(
				nullptr,
				0,
				&TraceHotkeyThread,
				nullptr,
				0,
				nullptr
			);

		if (!traceThread)
		{
			Logger::Error(
				"Failed to create combat trace hotkey thread."
			);

			return false;
		}

		CloseHandle(traceThread);

		return true;
	}

	bool IsActionOfferActive(
		HJAction action)
	{
		return
			action != HJAction::None &&
			g_buffer.offeredAction.load() ==
			action;
	}

	bool IsRightHandTransportActive()
	{
		return
			g_rightHandTransportOn.load() &&
			IsGameplayInputActive();
	}

	void NotifyAttackAccepted(
		std::uintptr_t container,
		std::uint32_t packedCommand)
	{
		// The native request is void: dispatch is observed, actual motion/hit is not... yadda yadda
		TraceCadence("attack-dispatched", packedCommand);
		g_attackTraceSerial.fetch_add(1);
		const std::uint32_t previousAttack =
			g_currentAttackCommand.load();

		const std::uint32_t sourceCommand =
			g_buffer.sourceCommand.load();

		const HJAction action =
			g_buffer.action.load();

		const HJAction requiredAction =
			GetCommandInputAction(
				packedCommand
			);

		if (requiredAction == HJAction::Tackle)
		{
			const ULONGLONG now =
				GetTickCount64();

			g_tackleStartedAt.store(
				now,
				std::memory_order_relaxed
			);

			g_tackleSequence.fetch_add(
				1,
				std::memory_order_relaxed
			);

			g_tackleActive.store(
				true,
				std::memory_order_release
			);
		}
		// mount and ground pound belong to the same tackle
		else if (packedCommand != 0x00170073 &&
			packedCommand != 0x00180073 &&
			packedCommand != 0x00190073)
		{
			g_tackleActive.store(
				false,
				std::memory_order_release
			);
		}

		// matched semantic input, buffer legitimately consumed.
		// this command legitimately consumed the buffer.
		if (action != HJAction::None &&
			requiredAction != HJAction::None &&
			requiredAction == action)
		{
			const std::uint64_t sequence =
				g_buffer.sequence.load();

			const ULONGLONG now =
				GetTickCount64();

			const ULONGLONG queuedAt =
				g_buffer.queuedAt.load();

			const ULONGLONG age =
				now >= queuedAt
				? now - queuedAt
				: 0;

			HJ::Logger::Info(
				std::format(
					"HJ BUFFER: consumed #{} {} "
					"command=0x{:X} age={}ms",
					sequence,
					GetHJActionName(action),
					packedCommand,
					age
				)
			);

			ClearBufferedHJAction();
		}

		// diff attack starteed before buffer belonging to prev attack was consumend
		else if (
			action != HJAction::None &&
			sourceCommand != 0 &&
			sourceCommand == previousAttack)
		{
			CancelBufferedAction();
		}

		// ALWAYS track new accepted attack including auto transitions yaddayadda
		g_currentAttackCommand.store(
			packedCommand
		);

		g_currentAttackContainer.store(
			container
		);

		g_currentAttackStartedAt.store(
			GetTickCount64(),
			std::memory_order_release
		);

		if (packedCommand == kHJRightHand1)
		{
			HJ::Logger::Info(
				"HJ RH1 COMMITMENT TEST: attack started"
			);
		}
	}
}