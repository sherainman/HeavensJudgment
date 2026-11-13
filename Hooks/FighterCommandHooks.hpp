#pragma once

#include <cstdint>

namespace HJ::Hooks::FighterCommand
{
	struct PadSnapshot
	{
		std::uint16_t mask = 0;
		std::uint8_t lt = 0;
		std::uint8_t rt = 0;
		bool active = false;
	};

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

	void NotifyPlayerBlockedHit() noexcept;

	void TickHitstopFrameScale() noexcept;

	PadSnapshot GetPadSnapshot() noexcept;
}