#pragma once


#include <cstddef>
#include <cstdint>

namespace HJ::HUD::HactOverlay
{

	struct HactRoute
	{
		std::uint32_t id;
		std::uint32_t prop;

	
		static constexpr std::uint32_t kDown        = 1u << 0;
		static constexpr std::uint32_t kDragon      = 1u << 1;
		static constexpr std::uint32_t kRun         = 1u << 2;
		static constexpr std::uint32_t kHeatMax     = 1u << 3;
		static constexpr std::uint32_t kKamae       = 1u << 4;
		static constexpr std::uint32_t kVRBattle    = 1u << 5;
		static constexpr std::uint32_t kDrunkLimit  = 1u << 6;
		static constexpr std::uint32_t kAgony       = 1u << 7;
		static constexpr std::uint32_t kHPPinch     = 1u << 8;
		static constexpr std::uint32_t kGuardBreak  = 1u << 9;
		static constexpr std::uint32_t kWallBound   = 1u << 10;
		static constexpr std::uint32_t kDownAo      = 1u << 11;
		static constexpr std::uint32_t kDownUtu     = 1u << 12;
		static constexpr std::uint32_t kRecoverDead = 1u << 13;
		static constexpr std::uint32_t kHeatOrDragon = 1u << 14;

		std::uint32_t requiredStates;
	};

	struct State
	{
		bool     available    = false; 
		std::uint32_t routeId  = 0;    
		std::uint32_t routeProp = 0;
		std::uint32_t latchedId = 0;    // raw latch slot value
		float     heatRatio   = 0.0f;   // fighter +0x28 / +0x2C
		int       tier        = 0;       // (int)heat, 0..6
		bool     rose         = false;   
		bool     masterEnable = false;   
		bool     suppressed   = false;   

		
		bool     resolved     = false;
	};

	const HactRoute* Routes();
	std::size_t RouteCount();


	const HactRoute* FindRoute(std::uint32_t id, std::uint32_t prop);


	State GetState();

	bool Initialize();
	void Shutdown();
}