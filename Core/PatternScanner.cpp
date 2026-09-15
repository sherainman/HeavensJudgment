#include "PatternScanner.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	struct PatternByte
	{
		std::uint8_t value = 0;
		bool wildcard = false;
	};

	std::vector<PatternByte> ParsePattern(std::string_view pattern)
	{
		std::vector<PatternByte> result;

		std::size_t i = 0;

		while (i < pattern.length())
		{
			if (pattern[i] == ' ')
			{
				++i;
				continue;
			}

			if (pattern[i] == '?')
			{
				result.push_back({
					0,
					true
					});

				++i;

				if (i < pattern.length() && pattern[i] == '?')
					++i;

				continue;
			}

			if (i + 1 >= pattern.length())
				break;

			const std::string byteString{
				pattern.substr(i, 2)
			};

			const auto value =
				static_cast<std::uint8_t>(
					std::stoul(byteString, nullptr, 16)
					);

			result.push_back({
				value,
				false
				});

			i += 2;
		}

		return result;
	}
}

namespace HJ::PatternScanner
{
	HJ::PatternScanner::Section HJ::PatternScanner::GetTextSection(
		HMODULE module)
	{
		if (!module)
			return {};

		auto* base =
			reinterpret_cast<std::uint8_t*>(module);

		auto* dosHeader =
			reinterpret_cast<IMAGE_DOS_HEADER*>(base);

		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
			return {};

		auto* ntHeaders =
			reinterpret_cast<IMAGE_NT_HEADERS64*>(
				base + dosHeader->e_lfanew
				);

		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
			return {};

		auto* section =
			IMAGE_FIRST_SECTION(ntHeaders);

		// 1st pref is conventional ".text" section.
		for (
			unsigned int i = 0;
			i < ntHeaders->FileHeader.NumberOfSections;
			++i)
		{
			char name[9]{};

			std::memcpy(
				name,
				section[i].Name,
				IMAGE_SIZEOF_SHORT_NAME
			);

			if (std::strcmp(name, ".text") == 0)
			{
				return {
					base + section[i].VirtualAddress,
					static_cast<std::size_t>(
						section[i].Misc.VirtualSize
					)
				};
			}
		}

		for (
			unsigned int i = 0;
			i < ntHeaders->FileHeader.NumberOfSections;
			++i)
		{
			const DWORD characteristics =
				section[i].Characteristics;

			const bool executable =
				(characteristics &
					IMAGE_SCN_MEM_EXECUTE) != 0;

			const bool containsCode =
				(characteristics &
					IMAGE_SCN_CNT_CODE) != 0;

			if (executable && containsCode)
			{
				return {
					base + section[i].VirtualAddress,
					static_cast<std::size_t>(
						section[i].Misc.VirtualSize
					)
				};
			}
		}

		return {};
	}

	std::uintptr_t Find(
		HMODULE module,
		std::string_view pattern)
	{
		const Section text =
			GetTextSection(module);

		if (!text)
			return 0;

		const auto parsed =
			ParsePattern(pattern);

		if (parsed.empty())
			return 0;

		if (parsed.size() > text.size)
			return 0;

		for (
			std::size_t i = 0;
			i <= text.size - parsed.size();
			++i)
		{
			bool matched = true;

			for (
				std::size_t j = 0;
				j < parsed.size();
				++j)
			{
				if (parsed[j].wildcard)
					continue;

				if (text.begin[i + j] != parsed[j].value)
				{
					matched = false;
					break;
				}
			}

			if (matched)
			{
				return reinterpret_cast<std::uintptr_t>(
					text.begin + i
					);
			}
		}

		return 0;
	}
}