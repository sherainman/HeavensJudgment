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

	bool IsCombatTraceActive();

	// stun latch for the evade gate: true between a player StunMode /
	// PlayerRagdollMode entry and the next WaitKamae / PlayerSway / Guard
	// (3 s hard expiry). fails open.
	bool IsPlayerStunLatched();

	bool IsRightHandTransportActive();

	void NotifyAttackAccepted(
		std::uintptr_t container,
		std::uint32_t packedCommand
	);
}
