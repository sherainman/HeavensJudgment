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

	constexpr ULONGLONG TraceDurationMs = 2000;

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

	// triggers wait around 5 frames may need to change if too high
	constexpr ULONGLONG kTriggerChordWindowMs = 83;

	struct TraceEntry
	{
		std::uint32_t key;
		std::uint32_t mode;
		std::uint8_t option;
		std::uintptr_t caller;
		bool eligible;

		bool operator==(const TraceEntry& other) const
		{
			return
				key == other.key &&
				mode == other.mode &&
				option == other.option &&
				caller == other.caller &&
				eligible == other.eligible;
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

			g_traceUntil.store(
				now + TraceDurationMs
			);

			{
				std::scoped_lock lock(g_traceMutex);
				g_traceEntries.clear();
			}

			HJ::Logger::Info(
				"=== COMBAT TRACE STARTED: "
				"2 second capture window ==="
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
		case 0x12: // RunRH1
		case 0x13: // RunRH2
			return HJAction::RightHand;

			// Left Hand
		case 0x05: // LH1
		case 0x06: // LH2
		case 0x07: // LH3
		case 0x0B: // RK2 -> RK3 uses Triangle
		case 0x0D: // RH1 -> LH branch
			return HJAction::LeftHand;

			// Right Leg
		case 0x09: // RK1
		case 0x0A: // RK2
		case 0x0E: // RH1_LH -> RK
		case 0x11: // RH1 -> RK
			return HJAction::RightLeg;

			// Left Leg/kicks/kneeswhatever
		case 0x0C: // LK1
		case 0x0F: // RH1_LH -> LK
		case 0x10: // RH1 -> LK
			return HJAction::LeftLeg;

		case 0x14: // TackleAcquire
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
			// virtual R1 Token
			*buttonMask |= 0x80;
			buttonValues[0] = 0xFF;
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

		if (now > expiresAt)
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

		if (controllerIndex != 0 ||
			buttonMask == nullptr ||
			buttonValues == nullptr)
		{
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

			ClearBufferedHJAction();

			return result;
		}

		XINPUT_STATE state{};

		if (XInputGetState(0, &state) != ERROR_SUCCESS)
		{
			g_previousShoulders = {};

			g_pendingTrigger =
				PendingTrigger::None;

			g_pendingTriggerStart = 0;

			ClearBufferedHJAction();

			return result;
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
		// both triggers need to cross the threshold during the same input for tackle 
		//
		const ULONGLONG now =
			GetTickCount64();

		//
		// both pressed on ssame update
		//
		if (rtPressed && ltPressed)
		{
			g_pendingTrigger =
				PendingTrigger::None;

			QueueHJAction(
				HJAction::Tackle
			);
		}
		//
		// trigger is waiting next trigger comes during the grace window
		//
		else if (
			g_pendingTrigger ==
			PendingTrigger::RightLeg &&
			ltPressed)
		{
			g_pendingTrigger =
				PendingTrigger::None;

			QueueHJAction(
				HJAction::Tackle
			);
		}
		else if (
			g_pendingTrigger ==
			PendingTrigger::LeftLeg &&
			rtPressed)
		{
			g_pendingTrigger =
				PendingTrigger::None;

			QueueHJAction(
				HJAction::Tackle
			);
		}
		//
		// starts waiting for the chord.,
		//
		else if (
			rtPressed &&
			g_pendingTrigger ==
			PendingTrigger::None)
		{
			g_pendingTrigger =
				PendingTrigger::RightLeg;

			g_pendingTriggerStart = now;
		}
		else if (
			ltPressed &&
			g_pendingTrigger ==
			PendingTrigger::None)
		{
			g_pendingTrigger =
				PendingTrigger::LeftLeg;

			g_pendingTriggerStart = now;
		}

		//
		// no 2nd trigger :(
		// play the kick action leg
		//
		if (g_pendingTrigger !=
			PendingTrigger::None &&
			now - g_pendingTriggerStart >=
			kTriggerChordWindowMs)
		{
			if (g_pendingTrigger ==
				PendingTrigger::RightLeg)
			{
				QueueHJAction(
					HJAction::RightLeg
				);
			}
			else
			{
				QueueHJAction(
					HJAction::LeftLeg
				);
			}

			g_pendingTrigger =
				PendingTrigger::None;
		}

		UpdateBufferedHJAction(
			buttonMask,
			buttonValues
		);

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
				g_lockOnEnabled.store(true);

				HJ::Logger::Info(
					"HJ LOCK ON: enabled on combat entry"
				);
			}
			else if (!fighting && previousFighting)
			{
				g_lockOnEnabled.store(false);

				HJ::Logger::Info(
					"HJ LOCK ON: disabled on combat exit"
				);
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
			const std::uint32_t endingCommand =
				g_currentAttackCommand.exchange(0);

			g_currentAttackContainer.store(0);

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
				<< "STATE TRACE"
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

		return g_namedStateHook.call<void*>(
			stateContainer,
			stateObject,
			stateName
		);
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
				eligible
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

	void ResetCurrentAttackTracking()
	{
		g_currentAttackCommand.store(0);
		g_currentAttackContainer.store(0);
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

	void NotifyAttackAccepted(
		std::uintptr_t container,
		std::uint32_t packedCommand)
	{
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
	}
}