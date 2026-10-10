#include "Hooks.hpp"

#include "FighterCommandHooks.hpp"
#include "ActionRequestsHooks.hpp"

#include "../HUD/RadialHealthBridge.hpp"
#include "../HUD/BoxingGaugeTrace.hpp"
#include "../Core/Logger.hpp"

namespace HJ::Hooks
{
	bool Initialize()
	{
		Logger::Info("Initializing hooks...");

		if (!FighterCommand::Initialize())
		{
			Logger::Error(
				"Failed to initialize FighterCommand hooks."
			);

			return false;
		}

		if (!HJ::HUD::RadialHealthBridge::Initialize())
		{
			Logger::Warning(
				"Radial health bridge didn initialize."
			);
		}

		if (!HJ::HUD::BoxingGaugeTrace::Initialize())
		{
			Logger::Warning(
				"Boxing gauge trace didn initialize."
			);
		}

		if (!ActionRequest::Initialize())
		{
			Logger::Error(
				"Failed to initialize ActionRequest hooks."
			);

			return false;
		}

		Logger::Info(
			"Hooks initialized successfully."
		);

		return true;
	}
}