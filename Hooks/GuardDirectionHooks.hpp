#pragma once

#include <cstdint>

namespace HJ::Hooks::GuardDirection
{
	// test build: gates enforce deny, rest logs only
	constexpr bool kDirectionalBlockEnabled = true;

	// reaction object dumps (noisy, anim RE only)
	constexpr bool kGuardReactionTelemetry = false;

	bool Initialize();

	// per-frame: swaps the motion_set guard triple to the held side
	void UpdateFromInput();

	// call on combat exit so the row never stays swapped
	void RestoreVanilla();

	// -1 none, 0 both, 1 left, 2 right. for the stance-restart pulse
	int CurrentSideId();

	// true once a live guard cell is adopted (dead mirrors excluded)
	bool HasGuardCells();

	// closed-loop stance restart: set on every player Guard entry,
	// consumed by the pulse waiter
	void NotifyGuardEntered();
	bool TakeGuardReentered();

	// walk owner->slot->entity->motion at Guard entry, scan the motion
	// object for the stance loop id (logging only, no writes)
	void ScanStanceMotion(std::uintptr_t owner, int side);

	// find the motion row through a live Guard state: every pointer
	// in the state object and its container is dereferenced (two
	// levels for object -> style -> row nesting) and each target
	// scored against known row values, offsets come from wherever
	// the triple actually sits (no file-layout assumption)
	void ScanGuardObject(std::uintptr_t stateObject, std::uintptr_t stateContainer);

	// hex-dump a live GuardReaction action object for motion-id hunting
	void DumpGuardReaction(std::uintptr_t object, int side);
}
