#include "Logger.hpp"

#include <Windows.h>

#include <fstream>
#include <mutex>
#include <string>

namespace
{
	std::ofstream g_logFile;
	std::mutex g_logMutex;

	void Write(const char* level, const std::string& message)
	{
		std::lock_guard lock(g_logMutex);

		if (!g_logFile.is_open())
			return;

		g_logFile
			<< "[" << level << "] "
			<< message
			<< '\n';

		g_logFile.flush();

	}

}

namespace HJ::Logger
{
	bool Initialize()
	{
		char dllPath[MAX_PATH]{};

		HMODULE module = nullptr;

		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&Initialize),
			&module))
		{
			return false;

		}

		if (!GetModuleFileNameA(
			module,
			dllPath,
			MAX_PATH))

		{
			return false;
		}

		std::string path = dllPath;

		const auto slash = path.find_last_of("\\/");

		if (slash != std::string::npos)
			path.resize(slash + 1);

		path += "HeavensJudgment.log";

		g_logFile.open(
			path,
			std::ios::out | std::ios::trunc
		);

		if (!g_logFile.is_open())
			return false;

		Write("INFO", "Heaven's Judgment logger initialized.");

		return true;
	}
	
	
	void Info(const std::string& message)
	{
		Write("INFO", message);
	}

	void Warning(const std::string& message)
	{
		Write("WARNING", message);
	}

	void Error(const std::string& message)
	{
		Write("ERROR", message);
	}

	void Shutdown()
	{
		std::lock_guard lock(g_logMutex);

		if (!g_logFile.is_open())
			return;

		g_logFile
			<< "[INFO] Heaven's Judgment shutting down.\n";

		g_logFile.flush();
		g_logFile.close();
	}
}