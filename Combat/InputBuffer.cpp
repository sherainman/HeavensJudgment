#include "InputBuffer.hpp"

#include <array>
#include <chrono>
#include <mutex>

namespace
{
	using Clock =
		std::chrono::steady_clock;

	using Milliseconds =
		std::chrono::milliseconds;

	std::array<
		HJ::Combat::InputBuffer::Entry,
		HJ::Combat::InputBuffer::Capacity
	> g_entries{};

	std::size_t g_head = 0;
	std::size_t g_count = 0;

	std::uint64_t g_nextSequence = 1;

	std::mutex g_bufferMutex;

	std::uint64_t GetCurrentTimeMs()
	{
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<Milliseconds>(
				Clock::now().time_since_epoch()
			).count()
			);
	}

	void RemoveFront()
	{
		if (g_count == 0)
			return;

		g_head =
			(g_head + 1) %
			HJ::Combat::InputBuffer::Capacity;

		--g_count;
	}

	void RemoveExpired(
		std::uint64_t now)
	{
		while (g_count != 0)
		{
			const auto& entry =
				g_entries[g_head];

			const std::uint64_t age =
				now - entry.pressedAtMs;

			if (age <=
				HJ::Combat::InputBuffer::LifetimeMs)
			{
				break;
			}

			RemoveFront();
		}
	}
}

namespace HJ::Combat::InputBuffer
{
	bool Push(Input input)
	{
		std::scoped_lock lock(
			g_bufferMutex
		);

		const std::uint64_t now =
			GetCurrentTimeMs();

		RemoveExpired(now);

		//
		// prefers newest intent
		// if the buffer completely fills discard oldest entry instead of refusing newest

		if (g_count == Capacity)
		{
			RemoveFront();
		}

		const std::size_t tail =
			(g_head + g_count) %
			Capacity;

		g_entries[tail] = {
			input,
			g_nextSequence++,
			now
		};

		++g_count;

		return true;
	}

	bool TryConsume(
		Entry& outEntry)
	{
		std::scoped_lock lock(
			g_bufferMutex
		);

		const std::uint64_t now =
			GetCurrentTimeMs();

		RemoveExpired(now);

		if (g_count == 0)
			return false;

		outEntry =
			g_entries[g_head];

		RemoveFront();

		return true;
	}

	void Clear()
	{
		std::scoped_lock lock(
			g_bufferMutex
		);

		g_head = 0;
		g_count = 0;
	}

	std::size_t Size()
	{
		std::scoped_lock lock(
			g_bufferMutex
		);

		RemoveExpired(
			GetCurrentTimeMs()
		);

		return g_count;
	}
}