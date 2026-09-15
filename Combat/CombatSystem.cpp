#include "CombatSystem.hpp"

#include "../Engine/Engine.hpp"

#include <atomic>
#include <string_view>

namespace HJ::Combat
{
	bool TryGetHeavyReplacement(
		std::uint32_t packedCommand,
		std::uint32_t& replacement)
	{
		switch (packedCommand)
		{
		case Commands::SnakeLight:
			replacement = Commands::SnakeHeavy;
			return true;

		case Commands::CraneLight:
			replacement = Commands::CraneHeavy;
			return true;

		case Commands::TigerLight:
			replacement = Commands::TigerHeavy;
			return true;

		case Commands::BoxerLightA:
		case Commands::BoxerLightB:
		case Commands::BoxerLightFollowup:
			replacement = Commands::BoxerHeavy;
			return true;

		default:
			return false;
		}
	}

	std::uint16_t GetStyleKey(
		std::uint32_t packedCommand)
	{
		return static_cast<std::uint16_t>(
			packedCommand & 0xFFFF
			);
	}

	std::uint16_t GetVariant(
		std::uint32_t packedCommand)
	{
		return static_cast<std::uint16_t>(
			packedCommand >> 16
			);
	}

	bool IsKnownPlayerAttack(
		std::uint32_t packedCommand)
	{
		switch (packedCommand)
		{
		case Commands::SnakeLight:
		case Commands::SnakeHeavy:

		case Commands::CraneLight:
		case Commands::CraneHeavy:
		case Commands::CraneExGrab:

		case Commands::TigerLight:
		case Commands::TigerHeavy:

		case Commands::BoxerLightA:
		case Commands::BoxerLightB:
		case Commands::BoxerLightFollowup:
		case Commands::BoxerHeavy:
			return true;

		default:
			return false;
		}
	}

	bool TryResolveInput(
		InputBuffer::Input input,
		std::uint32_t& packedCommand)
	{
		switch (input)
		{
		case InputBuffer::Input::RightTrigger:
			packedCommand =
				Commands::BoxerLightB;

			return true;

		case InputBuffer::Input::RightBumper:
			packedCommand =
				Commands::TigerHeavy;

			return true;

		case InputBuffer::Input::LeftTrigger:
			packedCommand =
				Commands::BoxerHeavy;

			return true;

		case InputBuffer::Input::LeftBumper:
			packedCommand =
				Commands::CraneExGrab;

			return true;

		default:
			return false;
		}
	}

	bool IsInCombat()
	{
		bool fighting = false;

		if (!HJ::Engine::TryGetFightingState(fighting))
			return false;

		return fighting;
	}
}