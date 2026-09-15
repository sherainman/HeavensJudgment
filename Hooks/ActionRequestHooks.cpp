#include "ActionRequestsHooks.hpp"
#include "../Combat/CombatSystem.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>
#include <safetyhook.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <sstream>

namespace
{
	SafetyHookInline g_attackBehaviorHook{};

	SafetyHookInline g_playerCombatUpdateHook{};

	std::atomic<std::uintptr_t>
		g_playerCombatObject{ 0 };

	std::atomic<std::uint32_t>
		g_pendingAttack{ 0 };

	std::atomic<bool> g_lightToHeavyTest{ false };

	void __fastcall AttackBehaviorHook(
		std::uintptr_t container,
		std::uint32_t packedCommand,
		const char* stateName)
	{
		const auto caller =
			reinterpret_cast<std::uintptr_t>(
				_ReturnAddress()
				);

		const std::uint32_t originalPackedCommand =
			packedCommand;

		const bool isPlayerAttack =
			caller == 0x142EFC750 &&
			stateName != nullptr &&
			std::strcmp(stateName, "Attack") == 0;

		if (isPlayerAttack)
		{
			g_playerCombatObject.store(
				container
			);

			std::ostringstream stream;

			stream
				<< "Captured player combat object: 0x"
				<< std::hex
				<< container;

			HJ::Logger::Info(
				stream.str()
			);
		}

		if (isPlayerAttack &&
			g_lightToHeavyTest.load())
		{
			std::uint32_t replacement = 0;

			if (isPlayerAttack)
			{
				g_playerCombatObject.store(
					container
				);
			}

			if (HJ::Combat::TryGetHeavyReplacement(
				packedCommand,
				replacement))
			{
				if (g_lightToHeavyTest.exchange(false))
				{
					packedCommand =
						replacement;

					std::ostringstream stream;

					stream
						<< "ATTACK OVERRIDE"
						<< " original=0x"
						<< std::hex
						<< originalPackedCommand
						<< " replacement=0x"
						<< replacement;

					HJ::Logger::Info(
						stream.str()
					);
				}
			}
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
				<< " original=0x"
				<< originalPackedCommand
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
	}

	std::uint8_t __fastcall PlayerCombatUpdateHook(
		std::uintptr_t combatObject)
	{
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

		const std::uint32_t pendingAttack =
			g_pendingAttack.exchange(0);

		if (pendingAttack == 0)
		{
			return result;
		}

		//
		// Player combat vtable +0xD0
		// = FUN_142F22340
		//
		using DispatchCommandFn =
			void(__fastcall*)(
				std::uintptr_t,
				std::uint32_t
				);

		const std::uintptr_t vtable =
			*reinterpret_cast<std::uintptr_t*>(
				combatObject
				);

		const auto dispatchCommand =
			*reinterpret_cast<DispatchCommandFn*>(
				vtable + 0xD0
				);

		{
			std::ostringstream stream;

			stream
				<< "HJ DIRECT ATTACK"
				<< " object=0x"
				<< std::hex
				<< combatObject
				<< " command=0x"
				<< pendingAttack
				<< " dispatcher=0x"
				<< reinterpret_cast<std::uintptr_t>(
					dispatchCommand
					);

			HJ::Logger::Info(
				stream.str()
			);
		}

		dispatchCommand(
			combatObject,
			pendingAttack
		);

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

		//
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

		const auto target =
			base + AttackBehaviorRva;

		const auto playerCombatUpdateTarget =
			base + PlayerCombatUpdateRva;

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

		return true;
	}

	bool RequestAttack(
		std::uint32_t packedCommand)
	{
		if (!HJ::Combat::IsInCombat())
		{
			Logger::Warning(
				"Cannot request attack: player is not in combat."
			);

			return false;
		}

		if (g_playerCombatObject.load() == 0)
		{
			Logger::Warning(
				"Cannot request attack: "
				"player combat object has not been captured yet."
			);

			return false;
		}

		g_pendingAttack.store(
			packedCommand
		);

		std::ostringstream stream;

		stream
			<< "Queued HJ attack command 0x"
			<< std::hex
			<< packedCommand;

		Logger::Info(
			stream.str()
		);

		return true;
	}

	void EnableLightToHeavyTest()
	{
		g_lightToHeavyTest.store(true);

		Logger::Info(
			"Next known player light attack will be replaced with heavy."
		);
	}
}