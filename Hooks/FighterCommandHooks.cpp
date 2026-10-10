#pragma comment(lib, "Xinput.lib")

#include "FighterCommandHooks.hpp"
#include "ActionRequestsHooks.hpp"

#include "../Core/Logger.hpp"
#include "../Engine/Engine.hpp"

#include <Windows.h>
#include <safetyhook.hpp>
#include <Xinput.h>

#include <cstdint>
#include <intrin.h>
#include <sstream>
#include <atomic>
#include <string>
#include <cstring>
#include <format>

#include <mutex>
#include <unordered_set>

// i am going to make an effort to comment this.
// i am going to make an effort to comment this. yadda yadda

namespace
{
	using HJAction =
		HJ::Hooks::FighterCommand::HJAction;

	SafetyHookInline g_checkEligibilityHook{};
	SafetyHookInline g_namedStateHook{};
	SafetyHookInline g_controllerTranslateHook{};

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

			//
			// AUTO TRANSIOTIONS ETCFCC.
			// LH4, tackle mount/ground pound/etc.
			//
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

		//
		// first transport update should be ON!!!!!
		//
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

		//
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
			g_previousShoulders = {};

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
			g_previousShoulders = {};

			g_pendingTrigger =
				PendingTrigger::None;

			g_pendingTriggerStart = 0;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;

			ClearBufferedHJAction();

			return result;
		}

		//
		// RH1 steering prototype
		//
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

					*leftStickX *= authority;
					*leftStickY *= authority;
				}
			}
		}


		const bool physicalRB =
			(state.Gamepad.wButtons &
				XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;

		const bool physicalLB =
			(state.Gamepad.wButtons &
				XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;

		const bool physicalRT =
			state.Gamepad.bRightTrigger >
			XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

		const bool physicalLT =
			state.Gamepad.bLeftTrigger >
			XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

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

		//
		// HJ owns the phys shoulder buttons
		//
		constexpr std::uint32_t kLTMask = 0x10;
		constexpr std::uint32_t kRTMask = 0x20;
		constexpr std::uint32_t kLBMask = 0x40;
		constexpr std::uint32_t kRBMask = 0x80;

		*buttonMask &= ~(
			kLTMask |
			kRTMask |
			kLBMask |
			kRBMask
			);

		buttonValues[4] = 0; // LT
		buttonValues[5] = 0; // RT
		buttonValues[6] = 0; // LB
		buttonValues[7] = 0; // RB

		// a newer hand press replaces any trigger still waiting for its chord
		if (rbPressed || lbPressed)
		{
			g_pendingTrigger = PendingTrigger::None;
			g_pendingTriggerStart = 0;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;
		}

		//
		// Hands. ;)
		//
		if (rbPressed)
		{
			QueueHJAction(
				HJAction::RightHand
			);
		}

		if (lbPressed)
		{
			QueueHJAction(
				HJAction::LeftHand
			);
		}

		//
		// Legs / tackle.
		//
		// 
		//
		// same update is always tackle; root inputs still get the chord window
		//
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

		if (rtPressed && ltPressed)
		{
			g_pendingTrigger = PendingTrigger::None;
			g_pendingTriggerSequence = 0;
			g_pendingTriggerCancelSequence = 0;

			QueueHJAction(HJAction::Tackle);
		}
		// a second trigger can replace the kick only while it is still queued
		else if (
			(g_pendingTrigger == PendingTrigger::RightLeg && ltPressed) ||
			(g_pendingTrigger == PendingTrigger::LeftLeg && rtPressed))
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

		//
		// lock on just holds vanilla r1
		//

		if (g_lockOnEnabled.load(
			std::memory_order_acquire))
		{
			*buttonMask |= kRBMask;
			buttonValues[7] = 0xFF;
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

		if (XInputGetState(
			0,
			&state) != ERROR_SUCCESS)
		{
			previousA = false;
			previousR3 = false;

			return;
		}

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

		//
		// r3 going to eventually be a toggle lock on
		//

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

	void* __fastcall NamedStateHook(
		std::uintptr_t stateContainer,
		void* stateObject,
		const char* stateName)
	{
		const auto caller =
			reinterpret_cast<std::uintptr_t>(
				_ReturnAddress()
				);

		if (stateName != nullptr &&
			std::strcmp(stateName, "WaitKamae") == 0 &&
			stateContainer ==
			g_currentAttackContainer.load())
		{
			TraceCadence("WaitKamae-request");
			const std::uint32_t endingCommand =
				g_currentAttackCommand.exchange(0);

			HJ::Hooks::FighterCommand::
				ResetCurrentAttackTracking();

			if (endingCommand != 0)
			{
				const std::uint32_t sourceCommand =
					g_buffer.sourceCommand.load();

				if (sourceCommand == endingCommand)
				{
					HJ::Hooks::FighterCommand::
						CancelBufferedAction();
				}

				HJ::Logger::Info(
					std::format(
						"HJ ATTACK END: command=0x{:X}",
						endingCommand
					)
				);
			}
		}

		if (IsTraceActive())
		{
			std::ostringstream stream;

			stream
				<< "STATE TRACE +" << std::dec
				<< GetTickCount64() - g_traceStartedAt.load() << "ms"
				<< " name=\""
				<< (stateName ? stateName : "<null>")
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
			result == stateObject &&
			stateName != nullptr &&
			std::strcmp(stateName, "PlayerSway") == 0)
		{
			HJ::Hooks::ActionRequest::
				NotifyEvadeAccepted(stateContainer);
		}

		return result;
	}

	// addresses:
	// ulonglong FUN_142E8F500(
	//  ulonglong param_1,
	//  uint      param_2, // fighter command key
	//  uint      param_3, // style
	//  uint      param_4  // option/behavior fl;ag?
	// );
	//
	//
	// style fightercommand key:
	// snake = 0x40
	// crane = 0x41
	// tiger = 0x42
	// boxer = 0x73
	//
	// stores at:
	// DAT_14430BE48 + 0xE8
	//
	// windosx x64
	// rcx = context
	// edx = key
	// r8d = mode
	// r9b = option
	//
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

		// 
		// xinput_pollandtranslatecontroller
		// 0x1419F2EC0 - 0x140000000
		// = 0x019F2EC0

		constexpr std::uintptr_t ControllerTranslateRva =
			0x019F2EC0;

		//
		// TEMPORARY hardcoded rva
		// 0x142E8F500 - 0x140000000
		// = 0x02E8F500
		//
		constexpr std::uintptr_t CheckEligibilityRva =
			0x02E8F500;

		//
		// FUN_1403B43D0
		// 0x1403B43D0 - 0x140000000
		// = 0x003B43D0
		//
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

		//
		// match semantic ibjmput
		// this command legitimately consumed the buffer.
		//
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

		//
		// diff attack starteed before buffer belonging to prev attack was consumend
		//
		else if (
			action != HJAction::None &&
			sourceCommand != 0 &&
			sourceCommand == previousAttack)
		{
			CancelBufferedAction();
		}

		//
		// ALWAYS track new accepted attack including auto transitions yaddayadda
		//
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
