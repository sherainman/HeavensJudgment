#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace HJ::PatternScanner
{
	struct Section
	{
		std::uint8_t* begin = nullptr;
		std::size_t size = 0;

		explicit operator bool() const
		{
			return begin != nullptr && size != 0;
		}
	};

	Section GetTextSection(HMODULE module);

	std::uintptr_t Find(
		HMODULE module,
		std::string_view pattern
	);
}