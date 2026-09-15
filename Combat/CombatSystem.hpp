#pragma once

#include <cstdint>

namespace HJ::Combat
{
	enum class Style : std::uint16_t
	{
		Snake = 0x40,
		Crane = 0x41,
		Tiger = 0x42,
		Boxer = 0x73
	};

	namespace Commands
	{
		// Snake

		constexpr std::uint32_t SnakeLight =
			0x000A0040;

		constexpr std::uint32_t SnakeHeavy =
			0x00380040;

		// Crane

		constexpr std::uint32_t CraneLight =
			0x00070041;

		constexpr std::uint32_t CraneHeavy =
			0x00210041;

		constexpr std::uint32_t CraneExGrab =
			0x001F0041;

		// Tiger

		constexpr std::uint32_t TigerLight =
			0x00160042;

		constexpr std::uint32_t TigerHeavy =
			0x00520042;

		// Boxer
		// both 0x07 and 0x08 work as light idkwhy?

		constexpr std::uint32_t BoxerLightA =
			0x00070073;

		constexpr std::uint32_t BoxerLightB =
			0x00080073;

		constexpr std::uint32_t BoxerLightFollowup =
			0x00090073;

		constexpr std::uint32_t BoxerHeavy =
			0x001C0073;
	}

	std::uint16_t GetStyleKey(
		std::uint32_t packedCommand
	);

	std::uint16_t GetVariant(
		std::uint32_t packedCommand
	);

	bool IsKnownPlayerAttack(
		std::uint32_t packedCommand
	);

	bool TryGetHeavyReplacement(
		std::uint32_t packedCommand,
		std::uint32_t& replacement
	);

	bool IsInCombat();
}