#pragma once

#include <cstdint>

namespace HJ::Hooks::FighterCommand
{
	enum class HJAction : std::uint8_t
	{
		None,
		RightHand,
		LeftHand,
		RightLeg,
		LeftLeg,
		Tackle
	};

	bool Initialize();

	void CancelBufferedAction();

	void ResetCurrentAttackTracking();

	void NotifyPlayerCombatUpdate();

	bool IsActionOfferActive(
		HJAction action
	);

	void NotifyAttackAccepted(
		std::uintptr_t container,
		std::uint32_t packedCommand
	);
}