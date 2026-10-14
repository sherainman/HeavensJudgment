#include "Hooks.hpp"

#include "FighterCommandHooks.hpp"
#include "ActionRequestsHooks.hpp"
#include "GuardDirectionHooks.hpp"

#include "../HUD/RadialHealthBridge.hpp"
#include "../HUD/BoxingGaugeTrace.hpp"
#include "../HUD/FrameOverlay.hpp"
#include "../HUD/RadialDiscDrive.hpp"
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

		if (!HJ::HUD::FrameOverlay::Initialize())
		{
			Logger::Warning(
				"Frame overlay didn initialize."
			);
		}

		// module base for the scene-liveness probe. Drive itself stays
		// parked; this is just the base address it needs to scan.
		HJ::HUD::RadialDiscDrive::Initialize();

		if (!ActionRequest::Initialize())
		{
			Logger::Error(
				"Failed to initialize ActionRequest hooks."
			);

			return false;
		}

		if (!GuardDirection::Initialize())
		{
			Logger::Error(
				"Failed to initialize GuardDirection hooks."
			);

			return false;
		}

		Logger::Info(
			"Hooks initialized successfully."
		);

		return true;
	}
}