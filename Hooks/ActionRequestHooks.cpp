#include "ActionRequestsHooks.hpp"
#include "GuardDirectionHooks.hpp"
#include "../Combat/CombatSystem.hpp"
#include "FighterCommandHooks.hpp"
#include "../HUD/RadialDisc.hpp"
#include "../HUD/HactOverlay.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>
#include <safetyhook.hpp>
#include <Xinput.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <sstream>
#include <format>
#include <cmath>
#include <string>

struct HJPlayerTransform
{
	float positionX;
	float positionY;
	float positionZ;

	float forwardX;
	float forwardY;
	float forwardZ;
};

namespace
{
	SafetyHookInline g_attackBehaviorHook{};

	SafetyHookInline g_playerCombatUpdateHook{};

	SafetyHookInline g_swayRequestHook{};
	SafetyHookMid g_exBoostInputHook{};

	SafetyHookInline g_queueStyleChangeActionHook{};

	constexpr std::uint32_t kBoxerStyleId = 1;

	constexpr std::uintptr_t kRuntimeStateGlobalRva =
		0x0430BE48;

	constexpr std::uintptr_t kCurrentStyleOffset =
		0x68;

	constexpr std::uintptr_t kQueueStyleChangeActionRva =
		0x02F1D410;

	std::atomic<std::uintptr_t>
		g_boxerInitializedObject{ 0 };

	std::atomic<bool> g_pendingEvade{ false };

	std::atomic<std::uintptr_t>
		g_evadeRequestObject{ 0 };

	std::atomic<std::uint64_t>
		g_evadeAcceptedSequence{ 0 };

	std::atomic<std::uintptr_t>
		g_playerCombatObject{ 0 };

	constexpr bool kOwnVanillaEvadeInput =
		true;

	constexpr std::uintptr_t kVanillaSwayCallerRva =
		0x02EEFB85;

	constexpr std::uint32_t kSnakeVanillaLight =
		0x000A0040;

	constexpr std::uint32_t kSnakeVanillaHeavy =
		0x00380040;

	constexpr std::uint32_t kSnakeVanillaGrab =
		0x002A0040;

	std::atomic<std::uintptr_t> g_playerEntity{ 0 };

	constexpr std::uintptr_t kEntityPointerTableRva =
		0x0430EDC0;

	constexpr std::uintptr_t kEntityGenerationTableRva =
		0x0430EDC8;

	constexpr std::uintptr_t kEntityTypeTableRva =
		0x0430EDCC;

	constexpr std::uintptr_t kPlayerPositionOffset =
		0x10F0;

	struct PlayerPosition
	{
		float x;
		float y;
		float z;
	};

	std::atomic<float> g_playerPositionX{ 0.0f };
	std::atomic<float> g_playerPositionY{ 0.0f };
	std::atomic<float> g_playerPositionZ{ 0.0f };

	std::atomic<float> g_playerForwardX{ 0.0f };
	std::atomic<float> g_playerForwardY{ 0.0f };
	std::atomic<float> g_playerForwardZ{ 1.0f };

	std::atomic<bool> g_playerTransformValid{ false };

	PlayerPosition g_previousPlayerPosition{};
	bool g_havePreviousPlayerPosition = false;

	// HJ_RightLeg_1
	// Boxer table variant 0x09, style key 0x73
	constexpr std::uint32_t kHJRightLeg1 =
		0x00090073;

	// the tackle
	// boxer and style key 0x73

	constexpr std::uint32_t kHJTackleAcquire =
		0x00160073;

	void __fastcall AttackBehaviorHook(
		std::uintptr_t container,
		std::uint32_t packedCommand,
		const char* stateName)
	{
		const auto caller =
			reinterpret_cast<std::uintptr_t>(
				_ReturnAddress()
				);

		const bool isPlayerAttack =
			caller == 0x142EFC750 &&
			stateName != nullptr &&
			std::strcmp(stateName, "Attack") == 0;


		// HJ right leg uses square as its internal token in fightercommand, rt synthesizes square 
		// but real square must not launch the kick too 
		if (isPlayerAttack &&
			packedCommand == kHJRightLeg1)
		{
			XINPUT_STATE input{};

			const bool hasController =
				XInputGetState(0, &input) == ERROR_SUCCESS;

			const bool physicalRT =
				hasController &&
				input.Gamepad.bRightTrigger >
				XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

			if (!physicalRT &&
				!HJ::Hooks::FighterCommand::IsActionOfferActive(
					HJ::Hooks::FighterCommand::HJAction::RightLeg))
			{
				HJ::Logger::Info(
					"HJ RIGHT LEG: physical Square route suppressed"
				);

				return;
			}
		}

		if (isPlayerAttack &&
			packedCommand == kHJTackleAcquire)
		{
			XINPUT_STATE input{};

			const bool hasController =
				XInputGetState(0, &input) == ERROR_SUCCESS;

			const bool physicalLT =
				hasController &&
				input.Gamepad.bLeftTrigger >
				XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

			const bool physicalRT =
				hasController &&
				input.Gamepad.bRightTrigger >
				XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

			const bool physicalTackleChord =
				physicalLT &&
				physicalRT;

			if (!physicalTackleChord &&
				!HJ::Hooks::FighterCommand::IsActionOfferActive(
					HJ::Hooks::FighterCommand::HJAction::Tackle))
			{
				HJ::Logger::Info(
					"HJ TACKLE: non-trigger-chord route suppressed"
				);

				return;
			}
		}

		const bool isVanillaSnakeCombatAction =
			packedCommand == kSnakeVanillaLight ||
			packedCommand == kSnakeVanillaHeavy ||
			packedCommand == kSnakeVanillaGrab;

		if (isPlayerAttack &&
			isVanillaSnakeCombatAction)
		{
			HJ::Logger::Info(
				std::format(
					"HJ VANILLA SNAKE ACTION SUPPRESSED command=0x{:X}",
					packedCommand
				)
			);

			return;
		}

		if (HJ::Hooks::FighterCommand::
			IsCombatTraceActive())
		{
			{
				const std::uint32_t commandKey =
					HJ::Hooks::ActionRequest::GetCommandKey(
						packedCommand
					);

				const std::uint32_t variant =
					HJ::Hooks::ActionRequest::GetVariant(
						packedCommand
					);

				std::ostringstream stream;

				stream
					<< "ATTACK REQUEST"
					<< " name=\""
					<< (stateName ? stateName : "<null>")
					<< "\""
					<< " container=0x"
					<< std::hex
					<< container
					<< " packed=0x"
					<< packedCommand
					<< " key=0x"
					<< commandKey
					<< " variant=0x"
					<< variant
					<< " caller=0x"
					<< caller;

				HJ::Logger::Info(
					stream.str()
				);
			}
		}

		g_attackBehaviorHook.call<void>(
			container,
			packedCommand,
			stateName
		);

		if (isPlayerAttack)
		{
			HJ::Hooks::FighterCommand::
				NotifyAttackAccepted(
					container,
					packedCommand
				);
		}
	}


	bool SafeReadByte(
		std::uintptr_t address,
		std::uint8_t& value) noexcept
	{
		__try
		{
			value =
				*reinterpret_cast<const std::uint8_t*>(
					address
					);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool SafeWriteByte(
		std::uintptr_t address,
		std::uint8_t value) noexcept
	{
		__try
		{
			*reinterpret_cast<std::uint8_t*>(
				address
				) = value;

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool SafeReadPointer(
		std::uintptr_t address,
		std::uintptr_t& value) noexcept
	{
		__try
		{
			value =
				*reinterpret_cast<const std::uintptr_t*>(
					address
					);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
			return false;
		}
	}

	bool SafeReadU32(
		std::uintptr_t address,
		std::uint32_t& value) noexcept
	{
		__try
		{
			value =
				*reinterpret_cast<const std::uint32_t*>(
					address
					);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
			return false;
		}
	}

	bool SafeReadI32(
		std::uintptr_t address,
		std::int32_t& value) noexcept
	{
		__try
		{
			value =
				*reinterpret_cast<const std::int32_t*>(
					address
					);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
			return false;
		}
	}

	// player-entity evade state for the deny table. +0x3C bit 6 is the
	// sway-eligible flag, +0x414 carries stun/knockdown state (ledger:
	// enemy scan expects 0x55 somewhere in there, player values unproven
	// so we log, never gate on these yet).
	struct EvadeEntityState
	{
		bool hasEntity = false;
		std::uintptr_t entity = 0;
		bool hasE3C = false;
		std::uint32_t e3c = 0;
		bool hasE414 = false;
		std::int32_t e414 = 0;
		bool hasE490 = false;
		std::int32_t e490 = 0;
	};

	EvadeEntityState ReadEvadeEntityState() noexcept
	{
		EvadeEntityState st;
		const std::uintptr_t entity =
			g_playerEntity.load(std::memory_order_acquire);
		if (entity == 0)
			return st;
		st.hasEntity = true;
		st.entity = entity;
		st.hasE3C = SafeReadU32(entity + 0x3C, st.e3c);
		st.hasE414 = SafeReadI32(entity + 0x414, st.e414);
		st.hasE490 = SafeReadI32(entity + 0x490, st.e490);
		return st;
	}


	// interim exclusion rule (proven 2026-10-13: stance does NOT move
	// e414 — it reads 1 locked-on or not). deny only on a deviating
	// entity state; stance alone never denies. stun/down/weapon values
	// still being mapped from these logs; until then anything nominal
	// behaves like the old always-allow path.
	bool EvadeEntityAbnormal(const EvadeEntityState& st) noexcept
	{
		return st.hasE414 && st.e414 != 1;
	}

	// arg split experiment: the game calls it with 1. logging 0
	// alongside tells us whether stance/stun split across args.
	bool ProbeSwayAllowed(
		std::uintptr_t combatObject,
		std::uint8_t mode)
	{
		if (combatObject == 0)
			return false;

		std::uintptr_t vtable = 0;

		if (!SafeReadPointer(
			combatObject,
			vtable) ||
			vtable == 0)
			return false;

		using EligibilityFn =
			std::uint8_t(__fastcall*)(
				std::uintptr_t,
				std::uint8_t
				);

		std::uintptr_t eligibilityAddress = 0;

		if (!SafeReadPointer(
			vtable + 0x208,
			eligibilityAddress) ||
			eligibilityAddress == 0)
			return false;

		const auto eligibility =
			reinterpret_cast<EligibilityFn>(
				eligibilityAddress
			);

		return eligibility(
			combatObject,
			mode
		) != 0;
	}

	bool ProbeNativeEvadeEligibility(
		std::uintptr_t combatObject)
	{
		return ProbeSwayAllowed(combatObject, 1);
	}

	bool TryReadEvadeStateFlag(
		std::uintptr_t stateRoot,
		std::uintptr_t offset,
		std::uint8_t& outFlag)
	{
		std::uintptr_t first = 0;
		std::uintptr_t second = 0;

		if (!SafeReadPointer(
			stateRoot + offset,
			first) ||
			first == 0)
		{
			return false;
		}

		if (!SafeReadPointer(
			first + 0x8,
			second) ||
			second == 0)
		{
			return false;
		}

		return SafeReadByte(
			second + 0x18,
			outFlag
		);
	}

	bool TryGetNativeEvadeStateRoot(
		std::uintptr_t combatObject,
		std::uintptr_t& outStateRoot) noexcept
	{
		outStateRoot = 0;

		if (combatObject == 0)
			return false;

		const auto moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr));

		if (moduleBase == 0)
			return false;

		using GetContextFn =
			std::uintptr_t(__fastcall*)(std::uintptr_t);

		using GetStateRootFn =
			std::uintptr_t(__fastcall*)(std::uintptr_t);

		const auto getContext =
			reinterpret_cast<GetContextFn>(
				moduleBase + 0x02ED1270);

		const auto getStateRoot =
			reinterpret_cast<GetStateRootFn>(
				moduleBase + 0x02E97FE0);

		if (!getContext || !getStateRoot)
			return false;

		std::uintptr_t contextVal = 0;

		__try
		{
			contextVal = getContext(combatObject + 0x30);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		if (contextVal == 0)
			return false;

		__try
		{
			outStateRoot = getStateRoot(contextVal);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			outStateRoot = 0;
			return false;
		}

		return outStateRoot != 0;
	}

	// transition watch: the frozen words (e414 stuck at 1 through
	// reported stuns) mean either the state lives elsewhere or the
	// entity pointer is stale. log every change so tester annotations
	// ("knocked down second time") map to signatures.
	struct EvadeWatchState
	{
		bool armed = false;
		std::uintptr_t entity = 0;
		std::uint32_t e3c = 0;
		std::int32_t e414 = 0;
		std::int32_t e490 = 0;
		std::uint8_t d88 = 0;
		std::uint8_t ba8 = 0;
		bool probe = false;
		bool probe0 = false;
	};
	EvadeWatchState g_watch;

	static bool SafeProbe(std::uintptr_t combatObject, std::uint8_t mode = 1) noexcept
	{
		__try
		{
			return ProbeSwayAllowed(combatObject, mode);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// THE evade gate: every evade door calls this, no door carries its
	// own rule. deny only on deviating entity state (stance excluded by
	// design). verbose lines behind the combat trace, denials always log.
	bool EvadeAllowed(std::uintptr_t combatObject, const char* door)
	{
		const bool probeOk = SafeProbe(combatObject);
		const EvadeEntityState estate = ReadEvadeEntityState();
		std::uintptr_t combatVtable = 0;
		SafeReadPointer(combatObject, combatVtable);
		const bool trace =
			HJ::Hooks::FighterCommand::IsCombatTraceActive();
		if (trace)
		{
			HJ::Logger::Info(
				std::format(
					"HJ EVADE CHECK: ent=0x{:X} vt=0x{:X} probe1={} probe0={} e3c=0x{:X}({}) bit6={} e414={}({}) e490={}({})",
					static_cast<unsigned long long>(estate.entity),
					static_cast<unsigned long long>(combatVtable),
					probeOk,
					SafeProbe(combatObject, 0),
					static_cast<unsigned int>(estate.e3c),
					estate.hasE3C,
					estate.hasE3C ? ((estate.e3c >> 6) & 1u) : 0u,
					static_cast<int>(estate.e414),
					estate.hasE414,
					static_cast<int>(estate.e490),
					estate.hasE490
				)
			);
		}
		if (!probeOk && EvadeEntityAbnormal(estate))
		{
			HJ::Logger::Info(
				std::format("HJ EVADE DENIED [{}]: abnormal entity state", door)
			);

			return false;
		}
		if (HJ::Hooks::FighterCommand::IsPlayerStunLatched())
		{
			HJ::Logger::Info(
				std::format("HJ EVADE DENIED [{}]: stun latch", door)
			);

			return false;
		}
		if (trace)
		{
			HJ::Logger::Info(
				std::format(
					"HJ EVADE ALLOWED [{}]: {}",
					door,
					probeOk ? "native probe" : "nominal entity, stance excluded"
				)
			);
		}

		return true;
	}


	static std::uintptr_t ResolveMotionHandle(std::uint32_t handle) noexcept
	{
		const std::uintptr_t base =
			reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (base == 0 || (handle & 0xFFFFF) >= 0x80000)
			return 0;
		__try
		{
			const std::uintptr_t slot = (handle & 0xFFFFF) * 0x20;
			if (*reinterpret_cast<const std::uint16_t*>(base + 0x430EDC8 + slot) != (handle >> 0x14))
				return 0;
			return *reinterpret_cast<std::uintptr_t*>(base + 0x430EDC0 + slot);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	static bool CopyMotionBytes(std::uintptr_t target, std::uint8_t* out, std::size_t n) noexcept
	{
		__try
		{
			std::memcpy(out, reinterpret_cast<const void*>(target), n);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	std::uint32_t g_lastScanMotH = 0;

	void HuntMotionIdOffset(std::uintptr_t entity) noexcept
	{
		std::uint32_t motH = 0;
		if (entity == 0 || !SafeReadU32(entity + 0xC4, motH) || motH == 0)
			return;
		if (motH == g_lastScanMotH)
			return;
		g_lastScanMotH = motH;
		const std::uintptr_t motion = ResolveMotionHandle(motH);
		if (motion == 0)
			return;
		static std::uint8_t mbuf[0x400];
		if (!CopyMotionBytes(motion, mbuf, sizeof(mbuf)))
			return;
		// 13919 = guard motion row the guard logs show live. offsets that
		// repeat across sessions are the id field.
		std::string hits;
		for (int off = 0; off + 2 <= 0x400; off += 2)
		{
			std::uint16_t v = 0;
			std::memcpy(&v, mbuf + off, 2);
			if (v == 13919)
				hits += std::to_string(off) + " ";
		}
		HJ::Logger::Info(
			std::format(
				"HJ EVADE MOTSCAN: motH={} motion=0x{:X} id13919@off={}",
				motH,
				static_cast<unsigned long long>(motion),
				hits.empty() ? "(none)" : hits
			)
		);
	}

	void WatchEvadeState(std::uintptr_t combatObject) noexcept
	{
		std::uintptr_t stateRoot = 0;
		if (!TryGetNativeEvadeStateRoot(combatObject, stateRoot))
			return;
		std::uint8_t d88 = 0xFF, ba8 = 0xFF;
		if (!TryReadEvadeStateFlag(stateRoot, 0xD88, d88))
			return;
		if (!TryReadEvadeStateFlag(stateRoot, 0xBA8, ba8))
			return;
		const EvadeEntityState estate = ReadEvadeEntityState();
		const bool probeOk = SafeProbe(combatObject);
		const bool probe0Ok = SafeProbe(combatObject, 0);
		std::uint32_t motH = 0;
		if (estate.hasEntity)
			SafeReadU32(estate.entity + 0xC4, motH);
		if (!g_watch.armed ||
			g_watch.entity != estate.entity ||
			g_watch.e3c != estate.e3c ||
			g_watch.e414 != estate.e414 ||
			g_watch.e490 != estate.e490 ||
			g_watch.d88 != d88 ||
			g_watch.ba8 != ba8 ||
			g_watch.probe != probeOk ||
			g_watch.probe0 != probe0Ok)
		{
			g_watch.armed = true;
			g_watch.entity = estate.entity;
			g_watch.e3c = estate.e3c;
			g_watch.e414 = estate.e414;
			g_watch.e490 = estate.e490;
			g_watch.d88 = d88;
			g_watch.ba8 = ba8;
			g_watch.probe = probeOk;
			g_watch.probe0 = probe0Ok;
			HJ::Logger::Info(
				std::format(
					"HJ EVADE WATCH: ent=0x{:X} e3c=0x{:X} e414={} e490={} D88={} BA8={} probe1={} probe0={} motH={}",
					static_cast<unsigned long long>(estate.entity),
					static_cast<unsigned int>(estate.e3c),
					static_cast<int>(estate.e414),
					static_cast<int>(estate.e490),
					static_cast<unsigned int>(d88),
					static_cast<unsigned int>(ba8),
					probeOk,
					probe0Ok,
					motH
				)
			);
		}
		// offset hunt runs only while guarding (known 13919 motion live)
		// so normal combat stays quiet. tester just guards a few times.
		if (HJ::Hooks::GuardDirection::CurrentSideId() != -1 &&
			estate.hasEntity)
			HuntMotionIdOffset(estate.entity);
	}

	// slef explanatory...

	bool NopCode(
		std::uintptr_t address,
		std::size_t size)
	{
		DWORD oldProtect = 0;

		if (!VirtualProtect(
			reinterpret_cast<void*>(address),
			size,
			PAGE_EXECUTE_READWRITE,
			&oldProtect))
		{
			return false;
		}

		std::memset(
			reinterpret_cast<void*>(address),
			0x90,
			size
		);

		FlushInstructionCache(
			GetCurrentProcess(),
			reinterpret_cast<void*>(address),
			size
		);

		DWORD unused = 0;

		VirtualProtect(
			reinterpret_cast<void*>(address),
			size,
			oldProtect,
			&unused
		);

		return true;
	}


	bool PatchBoxerAvailability(
		std::uintptr_t base)
	{
		const std::uintptr_t boxerCheck =
			base + 0x02EA0810;

		const std::uint8_t expected[] =
		{
			0x48, 0x89, 0x5C, 0x24, 0x10,
			0x56,
			0x48, 0x83, 0xEC, 0x40,
			0xB9, 0x49, 0x12, 0x00, 0x00,
			0xE8
		};

		if (std::memcmp(
			reinterpret_cast<void*>(boxerCheck),
			expected,
			sizeof(expected)) != 0)
		{
			HJ::Logger::Error(
				"boxer availability bytes dont match"
			);

			return false;
		}

		if (!NopCode(
			boxerCheck + 0x16,
			6))
		{
			return false;
		}

		if (!NopCode(
			boxerCheck + 0x27,
			6))
		{
			return false;
		}

		if (!NopCode(
			boxerCheck + 0x43,
			6))
		{
			return false;
		}

		HJ::Logger::Info(
			"boxer availability patched"
		);

		return true;
	}

	void ForceBoxerStyleState()
	{
		const auto base =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr)
				);

		if (base == 0)
			return;

		std::uintptr_t runtimeState = 0;

		__try
		{
			runtimeState =
				*reinterpret_cast<std::uintptr_t*>(
					base +
					kRuntimeStateGlobalRva
					);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return;
		}

		if (runtimeState == 0)
			return;

		__try
		{
			auto* style =
				reinterpret_cast<std::uint32_t*>(
					runtimeState +
					kCurrentStyleOffset
					);

			// dont touch garbage/loading state
			const std::uint32_t current =
				*style;

			if (current < 1 ||
				current > 4)
			{
				return;
			}

			// lock mem to boxer
			if (current != kBoxerStyleId)
			{
				*style = kBoxerStyleId;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return;
		}
	}

	void __fastcall QueueStyleChangeActionHook(
		std::uintptr_t owner,
		std::uint32_t requestedStyleId)
	{
		// if game requests anything otyher than boxer id 
		if (requestedStyleId != kBoxerStyleId)
		{
			HJ::Logger::Info(
				std::format(
					"HJ STYLE: overriding queue request {} -> {}",
					requestedStyleId,
					kBoxerStyleId
				)
			);

			requestedStyleId = kBoxerStyleId;
		}

		// call orgianl fucntion with modified id
		g_queueStyleChangeActionHook.call<void>(
			owner,
			requestedStyleId
		);
	}

	void TryInitializeBoxerStyle(
		std::uintptr_t combatObject)
	{
		if (combatObject == 0 ||
			!g_queueStyleChangeActionHook)
		{
			return;
		}

		if (g_playerEntity.load(
			std::memory_order_acquire
		) == 0)
		{
			return;
		}

		const std::uintptr_t initializedObject =
			g_boxerInitializedObject.load(
				std::memory_order_acquire
			);

		if (initializedObject ==
			combatObject)
		{
			return;
		}

		g_boxerInitializedObject.store(
			combatObject,
			std::memory_order_release
		);

		HJ::Logger::Info(
			"HJ STYLE: initializing Boxer"
		);

		// queue boxer natively
		g_queueStyleChangeActionHook.call<void>(
			combatObject,
			kBoxerStyleId
		);

		ForceBoxerStyleState();
	}

	bool TryReadPlayerPosition(
		std::uintptr_t entity,
		PlayerPosition& outPosition) noexcept
	{
		if (entity == 0)
			return false;

		__try
		{
			outPosition =
				*reinterpret_cast<
				const PlayerPosition*>(
					entity +
					kPlayerPositionOffset
					);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		return
			std::isfinite(outPosition.x) &&
			std::isfinite(outPosition.y) &&
			std::isfinite(outPosition.z);
	}

	bool UpdatePlayerTransform(
		std::uintptr_t entity)
	{
		PlayerPosition current{};

		if (!TryReadPlayerPosition(
			entity,
			current))
		{
			g_playerTransformValid.store(
				false,
				std::memory_order_release
			);

			return false;
		}


		if (g_havePreviousPlayerPosition)
		{
			const float dx =
				current.x -
				g_previousPlayerPosition.x;

			const float dz =
				current.z -
				g_previousPlayerPosition.z;

			const float distanceSquared =
				dx * dx +
				dz * dz;


			// only update when facing from believable movement
			// should give tackle approach direction without re yagamis
			// rotattion structure separately
			if (distanceSquared > 0.000001f &&
				distanceSquared < 4.0f)
			{
				const float distance =
					std::sqrt(
						distanceSquared
					);

				g_playerForwardX.store(
					dx / distance,
					std::memory_order_relaxed
				);

				g_playerForwardY.store(
					0.0f,
					std::memory_order_relaxed
				);

				g_playerForwardZ.store(
					dz / distance,
					std::memory_order_relaxed
				);
			}
		}


		g_previousPlayerPosition =
			current;

		g_havePreviousPlayerPosition =
			true;


		g_playerPositionX.store(
			current.x,
			std::memory_order_relaxed
		);

		g_playerPositionY.store(
			current.y,
			std::memory_order_relaxed
		);

		g_playerPositionZ.store(
			current.z,
			std::memory_order_relaxed
		);


		g_playerTransformValid.store(
			true,
			std::memory_order_release
		);

		return true;
	}

	std::uintptr_t ResolvePlayerEntity(
		std::uintptr_t combatObject) noexcept
	{
		if (!combatObject)
			return 0;

		__try
		{
			// FUN_142ED1270 received combatobject + 0x38
			const std::uintptr_t objectA =
				*reinterpret_cast<const std::uintptr_t*>(
					combatObject + 0x38);

			if (!objectA)
				return 0;

			const std::uintptr_t objectB =
				*reinterpret_cast<const std::uintptr_t*>(
					objectA + 0x3C8);

			if (!objectB)
				return 0;

			const std::uint32_t handle =
				*reinterpret_cast<const std::uint32_t*>(
					objectB + 0x0C);

			if (!handle)
				return 0;

			const std::uint32_t index =
				handle & 0xFFFFF;

			if (index >= 0x80000)
				return 0;

			const std::uintptr_t entryOffset =
				static_cast<std::uintptr_t>(index) * 0x20;

			const std::uintptr_t moduleBase =
				reinterpret_cast<std::uintptr_t>(
					GetModuleHandleW(nullptr));

			if (!moduleBase)
				return 0;

			const std::uint16_t generation =
				*reinterpret_cast<const std::uint16_t*>(
					moduleBase +
					kEntityGenerationTableRva +
					entryOffset);

			const std::int16_t type =
				*reinterpret_cast<const std::int16_t*>(
					moduleBase +
					kEntityTypeTableRva +
					entryOffset);

			if (generation !=
				static_cast<std::uint16_t>(
					handle >> 20))
			{
				return 0;
			}

			if (type != 3)
				return 0;

			return *reinterpret_cast<const std::uintptr_t*>(
				moduleBase +
				kEntityPointerTableRva +
				entryOffset);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// Unverified chain; any AV on a bad non-null pointer -> null.
			return 0;
		}
	}

	void __fastcall SwayRequestTraceHook(
		std::uintptr_t combatObject)
	{
		const auto caller =
			reinterpret_cast<std::uintptr_t>(
				_ReturnAddress()
				);

		const auto moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr)
				);

		const std::uintptr_t playerObject =
			g_playerCombatObject.load();

		const bool isCapturedPlayer =
			playerObject != 0 &&
			combatObject == playerObject;

		const bool isVanillaEvadeRequest =
			caller ==
			moduleBase +
			kVanillaSwayCallerRva;


		if (kOwnVanillaEvadeInput &&
			isCapturedPlayer &&
			isVanillaEvadeRequest &&
			HJ::Combat::IsInCombat())
		{
			XINPUT_STATE input{};

			const bool physicalA =
				XInputGetState(0, &input) == ERROR_SUCCESS &&
				(input.Gamepad.wButtons &
					XINPUT_GAMEPAD_A) != 0;

			// fc uses a synthetic cross for r1 inputs
			// only permit the de sway request when the player is actually pressing phys a
			if (!physicalA)
			{
				if (HJ::Hooks::FighterCommand::
					IsCombatTraceActive())
				{
					HJ::Logger::Info(
						"HJ VANILLA EVADE SUPPRESSED: no physical A"
					);
				}

				return;
			}

			if (!EvadeAllowed(combatObject, "vanilla"))
				return;
		}

		if (HJ::Hooks::FighterCommand::
			IsCombatTraceActive())
		{
			const auto message =
				std::format(
					"SWAY REQUEST TRACE object=0x{:X} caller=0x{:X}",
					static_cast<unsigned long long>(
						combatObject
						),
					static_cast<unsigned long long>(
						caller
						)
				);

			HJ::Logger::Info(
				message.c_str()
			);
		}

		const std::uintptr_t previousRequestObject =
			g_evadeRequestObject.exchange(
				isCapturedPlayer ? combatObject : 0,
				std::memory_order_acq_rel
			);

		g_swayRequestHook.call<void>(
			combatObject
		);

		g_evadeRequestObject.store(
			previousRequestObject,
			std::memory_order_release
		);
	}

	void ExBoostInputHook(SafetyHookContext& context)
	{
		const std::uintptr_t playerObject =
			g_playerCombatObject.load();

		if (playerObject != 0 &&
			context.rdi == playerObject &&
			HJ::Combat::IsInCombat() &&
			HJ::Hooks::FighterCommand::
				IsRightHandTransportActive())
		{
			// R2 still reaches fighter command, just not EX boost
			context.rax &= ~std::uintptr_t{ 1 };
		}
	}

	std::uint8_t __fastcall PlayerCombatUpdateHook(
		std::uintptr_t combatObject)
	{
		// Poll the HAct overlay from the per-frame combat tick. This is
		// what drives its heartbeat diagnostics and, once a renderer
		// exists, its draw state. Nothing else calls GetState(), so
		// without this the overlay is inert.
		HJ::HUD::HactOverlay::GetState();

		const std::uintptr_t previousPlayerObject =
			g_playerCombatObject.exchange(
				combatObject,
				std::memory_order_acq_rel
			);

		if (previousPlayerObject != combatObject)
		{
			g_pendingEvade.store(false);

			HJ::Hooks::FighterCommand::
				CancelBufferedAction();
			HJ::Hooks::FighterCommand::
				ResetCurrentAttackTracking();

			g_havePreviousPlayerPosition =
				false;

			g_playerTransformValid.store(
				false,
				std::memory_order_release
			);
		}

		// resolve yagamis entity only resolve when it changes or hasnt been successfully resolved yet

		if (previousPlayerObject != combatObject ||
			g_playerEntity.load(
				std::memory_order_acquire
			) == 0)
		{
			const std::uintptr_t playerEntity =
				ResolvePlayerEntity(
					combatObject
				);

			g_playerEntity.store(
				playerEntity,
				std::memory_order_release
			);


			if (playerEntity != 0)
			{
				HJ::Logger::Info(
					std::format(
						"Captured player entity: 0x{:X}",
						static_cast<unsigned long long>(
							playerEntity
							)
					)
				);
			}
		}


		const std::uint64_t previousEvadeSequence =
			g_evadeAcceptedSequence.load(
				std::memory_order_acquire
			);

		const std::uint8_t result =
			g_playerCombatUpdateHook.call<
			std::uint8_t
			>(
				combatObject
			);

		const std::uintptr_t playerObject =
			g_playerCombatObject.load();

		if (playerObject == 0 ||
			combatObject != playerObject)
		{
			return result;
		}

		ForceBoxerStyleState();

		const std::uintptr_t playerEntity =
			g_playerEntity.load(
				std::memory_order_acquire
			);

		if (playerEntity != 0)
		{
			if (!UpdatePlayerTransform(
				playerEntity))
			{
				g_playerEntity.store(
					0,
					std::memory_order_release
				);

				g_havePreviousPlayerPosition =
					false;
			}
		}

		HJ::Hooks::FighterCommand::
			NotifyPlayerCombatUpdate();

		// offset-hunt scaffolding: parked off the frame path. the probe
		// calls and the guarding-time scan cost more than they give back.
		constexpr bool kEvadeWatchEnabled = false;

		if (kEvadeWatchEnabled &&
			HJ::Combat::IsInCombat())
			WatchEvadeState(combatObject);

		static bool previousPhysicalA = false;

		XINPUT_STATE input{};

		const bool hasController =
			XInputGetState(0, &input) == ERROR_SUCCESS;

		if (hasController)
		{
			HJ::Hooks::GuardDirection::UpdateGuardButtons(
				input.Gamepad.wButtons);
		}

		const bool physicalA =
			hasController &&
			(input.Gamepad.wButtons &
				XINPUT_GAMEPAD_A) != 0;

		const bool physicalAPressed =
			physicalA &&
			!previousPhysicalA;

		previousPhysicalA =
			physicalA;

		const bool nativeEvadeAccepted =
			g_evadeAcceptedSequence.load(
				std::memory_order_acquire
			) != previousEvadeSequence;

		// relaxed-state snapshot: A pressed outside combat tells us what
		// the stance bits look like with no fight running. diff these
		// against the in-combat CHECK lines to isolate the stance flag,
		// which the gate must then ignore (free evade out of stance is
		// the feature, not the bug).
		if (physicalAPressed && !HJ::Combat::IsInCombat())
		{
			const bool probeOk =
				ProbeNativeEvadeEligibility(combatObject);

			const EvadeEntityState estate =
				ReadEvadeEntityState();

			// vtable pointer: unlocks the +0x208/+0x110 slot functions
			// for static decompile. logged, never branched on.
			std::uintptr_t combatVtable = 0;
			SafeReadPointer(combatObject, combatVtable);

			HJ::Logger::Info(
				std::format(
					"HJ EVADE RELAXED: ent=0x{:X} vt=0x{:X} probe={} e3c=0x{:X}({}) bit6={} e414={}({})",
					static_cast<unsigned long long>(estate.entity),
					static_cast<unsigned long long>(combatVtable),
					static_cast<unsigned long long>(estate.entity),
					probeOk,
					static_cast<unsigned int>(estate.e3c),
					estate.hasE3C,
					estate.hasE3C ? ((estate.e3c >> 6) & 1u) : 0u,
					static_cast<int>(estate.e414),
					estate.hasE414
				)
			);
		}

		if (physicalAPressed &&
			!nativeEvadeAccepted &&
			HJ::Combat::IsInCombat())
		{
			g_pendingEvade.store(true);

			HJ::Logger::Info(
				"HJ INPUT: physical A -> direct evade request"
			);
		}

		if (g_pendingEvade.exchange(false) &&
			!nativeEvadeAccepted &&
			HJ::Combat::IsInCombat())
		{
			std::uintptr_t stateRoot = 0;

			std::uint8_t flagD88 = 0xFF;
			std::uint8_t flagBA8 = 0xFF;

			const bool hasStateRoot =
				TryGetNativeEvadeStateRoot(
					combatObject,
					stateRoot
				);

			const bool hasD88 =
				hasStateRoot &&
				TryReadEvadeStateFlag(
					stateRoot,
					0xD88,
					flagD88
				);

			const bool hasBA8 =
				hasStateRoot &&
				TryReadEvadeStateFlag(
					stateRoot,
					0xBA8,
					flagBA8
				);

			if (HJ::Hooks::FighterCommand::
				IsCombatTraceActive())
			{
				HJ::Logger::Info(
					std::format(
						"HJ EVADE STATE: root=0x{:X} D88={} ({}) BA8={} ({})",
						static_cast<unsigned long long>(
							stateRoot
							),
						static_cast<unsigned int>(
							flagD88
							),
						hasD88,
						static_cast<unsigned int>(
							flagBA8
							),
						hasBA8
					)
				);
			}

			// every evade door shares one rule (see EvadeAllowed):
			// deny only on deviating entity state, stance excluded.
			if (!EvadeAllowed(combatObject, "direct"))
				return result;

			// direct A evade also works outside lock-on stance
			// the PlayerSway callback confirms whether it was accepted

			using HandleSwayRequestFn =
				void(__fastcall*)(std::uintptr_t);

			constexpr std::uintptr_t
				kHandleSwayRequestRva =
				0x02F1ED80;

			const auto moduleBase =
				reinterpret_cast<std::uintptr_t>(
					GetModuleHandleW(nullptr)
					);

			const auto handleSwayRequest =
				reinterpret_cast<HandleSwayRequestFn>(
					moduleBase +
					kHandleSwayRequestRva
					);

			HJ::Logger::Info(
				std::format(
					"HJ DIRECT EVADE object=0x{:X}",
					static_cast<unsigned long long>(
						combatObject
						)
				)
			);

			handleSwayRequest(
				combatObject
			);

			return result;
		}

		TryInitializeBoxerStyle(
			combatObject
		);

		return result;
	}
}

extern "C" __declspec(dllexport)
std::uintptr_t HJ_GetPlayerCombatObject()
{
	return g_playerCombatObject.load(
		std::memory_order_acquire
	);
}

extern "C" __declspec(dllexport)
std::uintptr_t HJ_GetPlayerEntity()
{
	return g_playerEntity.load(
		std::memory_order_acquire
	);
}

extern "C" __declspec(dllexport)
bool HJ_GetPlayerTransform(
	HJPlayerTransform* outTransform)
{
	if (outTransform == nullptr)
		return false;

	if (!g_playerTransformValid.load(
		std::memory_order_acquire))
	{
		return false;
	}


	outTransform->positionX =
		g_playerPositionX.load(
			std::memory_order_relaxed
		);

	outTransform->positionY =
		g_playerPositionY.load(
			std::memory_order_relaxed
		);

	outTransform->positionZ =
		g_playerPositionZ.load(
			std::memory_order_relaxed
		);


	outTransform->forwardX =
		g_playerForwardX.load(
			std::memory_order_relaxed
		);

	outTransform->forwardY =
		g_playerForwardY.load(
			std::memory_order_relaxed
		);

	outTransform->forwardZ =
		g_playerForwardZ.load(
			std::memory_order_relaxed
		);


	return true;
}


void HJ::Hooks::ActionRequest::TraceAttackObject(
	std::uintptr_t stateContainer,
	void* stateObject,
	const char* stateName,
	std::uintptr_t caller)
{
	if (stateObject == nullptr ||
		stateName == nullptr ||
		std::strcmp(stateName, "Attack") != 0 ||
		caller != 0x142F22305)
	{
		return;
	}

	const auto objectBytes =
		reinterpret_cast<std::uint8_t*>(
			stateObject
			);

	const std::uint32_t field78 =
		*reinterpret_cast<std::uint32_t*>(
			objectBytes + 0x78
			);

	const std::int32_t fieldB4 =
		*reinterpret_cast<std::int32_t*>(
			objectBytes + 0xB4
			);

	const std::uint8_t fieldC7 =
		*(objectBytes + 0xC7);

	std::ostringstream stream;

	stream
		<< "ATTACK OBJECT"
		<< " container=0x"
		<< std::hex
		<< stateContainer
		<< " object=0x"
		<< reinterpret_cast<std::uintptr_t>(
			stateObject
			)
		<< " +78=0x"
		<< field78
		<< " +B4=0x"
		<< fieldB4
		<< " +C7=0x"
		<< static_cast<unsigned int>(
			fieldC7
			);

	HJ::Logger::Info(
		stream.str()
	);
}

namespace HJ::Hooks::ActionRequest
{
	bool Initialize()
	{
		Logger::Info(
			"Initializing action request hooks..."
		);

		const HMODULE gameModule =
			GetModuleHandleA(nullptr);

		if (!gameModule)
		{
			Logger::Error(
				"ActionRequest hook: failed to get game module."
			);

			return false;
		}

		const auto base =
			reinterpret_cast<std::uintptr_t>(
				gameModule
				);

		
		if (!PatchBoxerAvailability(
			base))
		{
			Logger::Error(
				"failed to patch boxer availability"
			);

			return false;
		}


		// StartAttackBehaviorFromPackedCommand


		constexpr std::uintptr_t AttackBehaviorRva =
			0x02F21D80;

		// PlayerCombat_Update


		constexpr std::uintptr_t PlayerCombatUpdateRva =
			0x02EC3830;

		constexpr std::uintptr_t kSwayRequestRva =
			0x02F1ED80;

		// native boost input, after reading R2 and before and al, 1
		constexpr std::uintptr_t kExBoostInputRva =
			0x02EBFA18;


		const auto target =
			base + AttackBehaviorRva;

		const auto playerCombatUpdateTarget =
			base + PlayerCombatUpdateRva;

		const auto swayRequestAddress =
			base + kSwayRequestRva;

		const auto exBoostInputAddress =
			base + kExBoostInputRva;

		constexpr std::uint8_t exBoostInputBytes[] =
		{
			0x0F, 0xB6, 0x81, 0x25, 0x14, 0x00, 0x00,
			0x24, 0x01, 0x48, 0x83, 0xC4, 0x20,
			0x5F, 0x5E, 0x5B, 0xC3
		};

		if (std::memcmp(
			reinterpret_cast<const void*>(exBoostInputAddress - 7),
			exBoostInputBytes,
			sizeof(exBoostInputBytes)) != 0)
		{
			Logger::Error(
				"EX boost input signature does not match."
			);

			return false;
		}

		g_exBoostInputHook =
			safetyhook::create_mid(
				reinterpret_cast<void*>(exBoostInputAddress),
				&ExBoostInputHook
			);

		if (!g_exBoostInputHook)
		{
			Logger::Error(
				"Failed to install EX boost input hook."
			);

			return false;
		}

		Logger::Info(
			"EX boost input hook installed."
		);

		{
			std::ostringstream stream;

			stream
				<< "Installing action request hook at 0x"
				<< std::hex
				<< target;

			Logger::Info(
				stream.str()
			);
		}

		g_attackBehaviorHook =
			safetyhook::create_inline(
				reinterpret_cast<void*>(target),
				reinterpret_cast<void*>(
					&AttackBehaviorHook
					)
			);

		Logger::Info(
			"Action request hook installed."
		);

		{
			std::ostringstream stream;

			stream
				<< "Installing player combat update hook at 0x"
				<< std::hex
				<< playerCombatUpdateTarget;

			Logger::Info(
				stream.str()
			);
		}

		g_playerCombatUpdateHook =
			safetyhook::create_inline(
				reinterpret_cast<void*>(
					playerCombatUpdateTarget
					),
				reinterpret_cast<void*>(
					&PlayerCombatUpdateHook
					)
			);

		Logger::Info(
			"Player combat update hook installed."
		);

		{
			std::ostringstream stream;

			stream
				<< "Installing sway request trace hook at 0x"
				<< std::hex
				<< swayRequestAddress;

			Logger::Info(
				stream.str()
			);
		}

		g_swayRequestHook =
			safetyhook::create_inline(
				reinterpret_cast<void*>(
					swayRequestAddress
					),
				reinterpret_cast<void*>(
					&SwayRequestTraceHook
					)
			);

		if (!g_swayRequestHook)
		{
			Logger::Error(
				"Failed to install sway request trace hook."
			);

			return false;
		}

		Logger::Info(
			"Sway request trace hook installed."
		);

		return true;

		const auto queueStyleChangeAddress =
			base + kQueueStyleChangeActionRva;

		{
			std::ostringstream stream;
			stream
				<< "Installing QueueStyleChangeAction hook at 0x"
				<< std::hex
				<< queueStyleChangeAddress;
			Logger::Info(stream.str());
		}

		g_queueStyleChangeActionHook =
			safetyhook::create_inline(
				reinterpret_cast<void*>(queueStyleChangeAddress),
				reinterpret_cast<void*>(&QueueStyleChangeActionHook)
			);

		if (!g_queueStyleChangeActionHook)
		{
			Logger::Error(
				"Failed to install QueueStyleChangeAction hook."
			);

			return false;
		}

		Logger::Info(
			"QueueStyleChangeAction hook installed."
		);
	}

	void NotifyEvadeAccepted(
		std::uintptr_t stateContainer)
	{
		const std::uintptr_t playerObject =
			g_playerCombatObject.load(
				std::memory_order_acquire
			);

		if (playerObject == 0 ||
			stateContainer != playerObject ||
			g_evadeRequestObject.load(
				std::memory_order_acquire
			) != playerObject)
		{
			return;
		}

		HJ::Hooks::FighterCommand::
			CancelBufferedAction();

		HJ::Hooks::FighterCommand::
			ResetCurrentAttackTracking();

		g_evadeAcceptedSequence.fetch_add(
			1,
			std::memory_order_acq_rel
		);

		HJ::Logger::Info(
			"HJ EVADE ACCEPTED: PlayerSway queued"
		);
	}

	bool RequestEvade()
	{
		if (!HJ::Combat::IsInCombat())
		{
			HJ::Logger::Warning(
				"Cannot request evade: player is not in combat."
			);

			return false;
		}

		if (g_playerCombatObject.load() == 0)
		{
			HJ::Logger::Warning(
				"Cannot request evade: player combat object is not captured."
			);

			return false;
		}

		g_pendingEvade.store(true);

		HJ::Logger::Info(
			"HJ EVADE QUEUED"
		);

		return true;
	}
}