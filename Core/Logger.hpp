#pragma once

#include <string>

namespace HJ::Logger
{
	bool Initialize();

	void Info(const std::string& message);
	void Warning(const std::string& message);
	void Error(const std::string& message);

	void Shutdown();
}