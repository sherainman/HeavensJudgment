#include "ActionRequestsHooks.hpp"
#include "../Combat/CombatSystem.hpp"
#include "FighterCommandHooks.hpp"

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

namespace
{
	SafetyHookInline g_attackBehaviorHook{};

	SafetyHookInline g_playerCombatUpdateHook{};

	SafetyHookInline g_swayRequestHook{};

	std::atomic<bool> g_pendingEvade{ false };

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

	constexpr std::uintptr_t kCombatStyleOffset =
		0x2B8;

	constexpr std::uint8_t kBoxerCombatStyle =
		5;

	// 
	// HJ_RightLeg_1
	// 
	// Boxer table variant 0xD5, style key 0x73
	//
	constexpr std::uint32_t kHJRightLeg1 =
		0x00D50073;

	// the tackle
	// boxer and style key 0x73

	constexpr std::uint32_t kHJTackleAcquire =
		0x00DC0073;

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


		//
		// HJ right leg uses square as its internal token in fightercommand, rt synthesizes square 
		// but real square must not launch the kick too 
		//
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

			if (!physicalRT)
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
				XInputGetState(
					0,
					&input
				) == ERROR_SUCCESS;

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

			if (!physicalTackleChord)
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

	bool ProbeNativeEvadeEligibility(
		std::uintptr_t combatObject)
	{
		if (combatObject == 0)
			return false;

		const auto vtable =
			*reinterpret_cast<const std::uintptr_t*>(
				combatObject
				);

		if (vtable == 0)
			return false;

		using EligibilityFn =
			std::uint8_t(__fastcall*)(
				std::uintptr_t,
				std::uint32_t
				);

		const auto eligibility =
			*reinterpret_cast<EligibilityFn const*>(
				vtable + 0x208
				);

		if (eligibility == nullptr)
			return false;

		return eligibility(
			combatObject,
			1
		) != 0;
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
	std::uintptr_t& outStateRoot)
{
	outStateRoot = 0;

	if (combatObject == 0)
		return false;

	const auto moduleBase =
		reinterpret_cast<std::uintptr_t>(
			GetModuleHandleW(nullptr)
		);

	if (moduleBase == 0)
		return false;

	using GetContextFn =
		std::uintptr_t(__fastcall*)(
			std::uintptr_t
		);

	using GetStateRootFn =
		std::uintptr_t(__fastcall*)(
			std::uintptr_t
		);

	const auto getContext =
		reinterpret_cast<GetContextFn>(
			moduleBase + 0x02ED1270
		);

	const auto getStateRoot =
		reinterpret_cast<GetStateRootFn>(
			moduleBase + 0x02E97FE0
		);

	const std::uintptr_t context =
		getContext(
			combatObject + 0x30
		);

	if (context == 0)
		return false;

	outStateRoot =
		getStateRoot(
			context
		);

	return outStateRoot != 0;
}

	void ForceBoxerStyle(
		std::uintptr_t combatObject)
	{
		if (combatObject == 0)
			return;

		const std::uintptr_t styleAddress =
			combatObject + kCombatStyleOffset;

		std::uint8_t currentStyle = 0;

		if (!SafeReadByte(
			styleAddress,
			currentStyle))
		{
			return;
		}

		//
		// Known Lost Judgment Yagami style values:
		//
		// refuse to write if it doesnt look like the expected combat object
		// tiger = 2, crane = 3, snake = 4, boxer = 5

		if (currentStyle < 2 ||
			currentStyle > 5)
		{
			return;
		}

		if (currentStyle == kBoxerCombatStyle)
			return;

		if (!SafeWriteByte(
			styleAddress,
			kBoxerCombatStyle))
		{
			return;
		}

		HJ::Logger::Info(
			std::format(
				"HJ STYLE FORCE: {} -> Boxer (5)",
				static_cast<unsigned int>(
					currentStyle
					)
			)
		);
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
				XInputGetState(
					0,
					&input
				) == ERROR_SUCCESS &&
				(input.Gamepad.wButtons &
					XINPUT_GAMEPAD_A) != 0;

			//
			// fc uses a synthetic cross for r1 inputs
			//
			// only permit the de sway request when the player is actually pressing phys a
			//
			if (!physicalA)
			{
				HJ::Logger::Info(
					"HJ VANILLA EVADE SUPPRESSED: no physical A"
				);

				return;
			}

			HJ::Logger::Info(
				"HJ EVADE ALLOWED: native eligibility + physical A"
			);
		}

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

		g_swayRequestHook.call<void>(
			combatObject
		);
	}

	std::uint8_t __fastcall PlayerCombatUpdateHook(
		std::uintptr_t combatObject)
	{
		const std::uintptr_t previousPlayerObject =
			g_playerCombatObject.exchange(
				combatObject
			);

		if (previousPlayerObject != combatObject)
		{
			HJ::Logger::Info(
				std::format(
					"Captured player combat object from update: 0x{:X}",
					static_cast<unsigned long long>(
						combatObject
						)
				)
			);
		}

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

		static bool previousPhysicalA = false;

		XINPUT_STATE input{};

		const bool hasController =
			XInputGetState(
				0,
				&input
			) == ERROR_SUCCESS;

		const bool physicalA =
			hasController &&
			(input.Gamepad.wButtons &
				XINPUT_GAMEPAD_A) != 0;

		const bool physicalAPressed =
			physicalA &&
			!previousPhysicalA;

		previousPhysicalA =
			physicalA;

		if (physicalAPressed &&
			HJ::Combat::IsInCombat())
		{
			HJ::Hooks::FighterCommand::
				CancelBufferedAction();

			HJ::Hooks::FighterCommand::
				ResetCurrentAttackTracking();

			g_pendingEvade.store(true);

			HJ::Logger::Info(
				"HJ INPUT: physical A -> direct evade request"
			);
		}

		if (g_pendingEvade.exchange(false))
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

		return result;
	}
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

		



		// StartAttackBehaviorFromPackedCommand
		// 0x142F21D80 - 0x140000000
		// = 0x02F21D80
		//
		constexpr std::uintptr_t AttackBehaviorRva =
			0x02F21D80;

		//
		// PlayerCombat_Update
		// 0x142EC3830 - 0x140000000
		// = 0x02EC3830
		//

		constexpr std::uintptr_t PlayerCombatUpdateRva =
			0x02EC3830;

		constexpr std::uintptr_t kSwayRequestRva =
			0x02F1ED80;


		const auto target =
			base + AttackBehaviorRva;

		const auto playerCombatUpdateTarget =
			base + PlayerCombatUpdateRva;

		const auto swayRequestAddress =
			base + kSwayRequestRva;

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