#pragma once

#include <cstdint>

namespace HJ::HUD::RadialDiscDrive
{
	// drives our radial HP layer (scene 301, authored with the native
	// radial marker) using the engine's own set_value wrapper.
	// ABI: RCX = &u64 handle, XMM1 = value. hook sits right after the
	// controller's own set_value calls so no uiManager lock is held.
	void Initialize();

	void Drive(float ratio);

	// true while any life-gauge scene object is alive. throttled scan,
	// present hook calls it every frame.
	bool IsSceneAlive();

	// root +0x1C flag, bit 0x100 = hidden. missing root fails open.
	bool IsHudShown();

	// one-shot raw dump of live life-gauge objects. capture per ui
	// state (hud, phone, pause, title) and diff.
	void DumpSceneObjects();
}
