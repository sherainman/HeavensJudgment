#include "HeavensJudgment.hpp"

#include "Core/Logger.hpp"
#include "Core/PatternScanner.hpp"
#include "Hooks/Hooks.hpp"

#include <Windows.h>

#include <cstdint>
#include <sstream>

namespace HJ
{
	DWORD WINAPI Initialize(LPVOID parameter)
	{
		(void)parameter;

		if (!Logger::Initialize())
			return 0;

		Logger::Info(
			"Heaven's Judgment starting."
		);

		const HMODULE gameModule =
			GetModuleHandleA(nullptr);

		if (!gameModule)
		{
			Logger::Error(
				"Failed to get main executable module."
			);

			return 0;
		}

		// log lj base addresss
		{
			std::ostringstream stream;

			stream
				<< "Main executable base: 0x"
				<< std::hex
				<< reinterpret_cast<std::uintptr_t>(
					gameModule
					);

			Logger::Info(stream.str());
		}

		// locate lost judgment exec code section

		const auto textSection =
			PatternScanner::GetTextSection(
				gameModule
			);

		if (!textSection)
		{
			Logger::Warning(
				"Could not locate Lost Judgment executable code section."
			);
		}
		else
		{
			const auto textStart =
				reinterpret_cast<std::uintptr_t>(
					textSection.begin
					);

			const auto textEnd =
				reinterpret_cast<std::uintptr_t>(
					textSection.begin +
					textSection.size
					);

			std::ostringstream stream;

			stream
				<< "Lost Judgment code section: 0x"
				<< std::hex
				<< textStart
				<< " - 0x"
				<< textEnd
				<< " (size: 0x"
				<< textSection.size
				<< ")";

			Logger::Info(stream.str());
		}

		// init runtime hooks

		if (!Hooks::Initialize())
		{
			Logger::Error(
				"Hook initialization failed."
			);

			return 0;
		}

		Logger::Info(
			"Heaven's Judgment initialized successfully."
		);

		return 0;
	}
}