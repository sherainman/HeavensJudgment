#pragma once

#include <cstdint>
#include <cstddef>

namespace HJ::Combat::InputBuffer
{
	enum class Input : std::uint8_t
	{
		RightTrigger,
		RightBumper,
		LeftTrigger,
		LeftBumper
	};

	struct Entry
	{
		Input input;
		std::uint64_t sequence;
		std::uint64_t pressedAtMs;
	};

	constexpr std::size_t Capacity = 16;


	// the length an input will wait for the system to accept it before it becomes stale
	// start point only gonna tune it based on how it feels

	constexpr std::uint64_t LifetimeMs = 220;

	bool Push(Input input);

	bool TryConsume(
		Entry& outEntry
	);

	void Clear();

	std::size_t Size();

}