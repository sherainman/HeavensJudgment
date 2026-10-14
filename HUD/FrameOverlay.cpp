// FrameOverlay: hp disc drawn inside the games own frame.
//
// hooks Present + ExecuteCommandLists (resolved off dummy devices at
// init, so no game objects touched) and appends a tiny fan list to the
// games own submissions. dies quietly with a log line if anything fails.

#include "FrameOverlay.hpp"

#include "RadialHealthBridge.hpp"
#include "RadialDiscDrive.hpp"

#include "../Core/Logger.hpp"
#include "../Engine/Engine.hpp"

#include <Windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	constexpr wchar_t kIniName[] = L"DiscOverlay.ini";
	constexpr int kMaxBuffers = 4;
	constexpr int kRecSlots = 8;      // recorded-list cache (see below)
	constexpr int kSlotVerts = 4096; // rings + washes add up fast
	constexpr int kMaxVbVerts = kRecSlots * kSlotVerts;
	constexpr std::uint64_t kSlotMinAge = 3; // presents before reuse
	constexpr double kPi = 3.14159265358979323846;

	// locked anchor: the vanilla hub doesnt move (corner-anchored hud,
	// height-scaled), so these screenshot-calibrated fractions hold at any
	// res. players never touch em; ini keys are just fine-tune deltas.
	constexpr double kAutoCX = 0.131;
	constexpr double kAutoBY = 0.128;
	constexpr double kAutoR = 0.0316;

	struct Settings
	{
		double centerXH = 0.1300;
		double bottomYH = 0.1300;
		double legacyX = -1.0; // old CenterX key
		double legacyY = -1.0; // old CenterY key
		double radiusH = 0.0330100;
		double thick1080 = 0.0;
		std::uint32_t ringArgb = 0xFFFFFFFF;
		std::uint32_t trackArgb = 0x80342F22;
		std::uint32_t baseArgb = 0x00000000;
		std::uint8_t globalAlpha = 255;
		bool enabled = true;
		bool fightingOnly = false;
		double holeFrac = 0.0;
		double featherPx1080 = 3.0; // edge softness
		int style = 1;
		bool autoAnchor = true;
		double anchorDXH = 0.0; // manual nudge, normally stays 0
		double anchorDYH = 0.0;
		double radiusMul = 1.0;
		double lowHPFrac = 0.25; // below this the disc goes red + pulses
		std::uint32_t lowOuterArgb = 0xFFFF7A85; // light red at the rim
		std::uint32_t lowInnerArgb = 0xFF8A1020; // dark red at the hub
		double pulseHz = 0.5; // white<->red speed. 0 = steady red
		double dmgGhostSec = 1.0; // trailing red drains over this long
		std::uint32_t ghostArgb = 0x80E02030; // lost hp, half opacity red
		double dmgPulseSec = 0.6; // groove flash length on damage
		std::uint32_t dmgPulseArgb = 0x70FF2030;
		double healSec = 0.7; // green shine length on heal, one pulse
		std::uint32_t healArgb = 0x9030E060;
		std::uint32_t hubRingArgb = 0xFFFFFFFF; // center white ring
		double hubRing1080 = 2.0; // its thickness in px at 1080p
		int grooveCount = 5; // lit groove rings on damage/heal
		double groovePx1080 = 1.5; // their thickness in px at 1080p
		double grooveOuterMul = 2.5; // band outer edge, x the disc radius
	};

	HMODULE OwnModule()
	{
		MEMORY_BASIC_INFORMATION mbi{};
		VirtualQuery(&OwnModule, &mbi, sizeof(mbi));
		return static_cast<HMODULE>(mbi.AllocationBase);
	}

	std::wstring IniPath()
	{
		wchar_t dir[MAX_PATH]{};
		GetModuleFileNameW(OwnModule(), dir, MAX_PATH);
		std::wstring p(dir);
		const std::size_t slash = p.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			p.resize(slash + 1);
		return p + kIniName;
	}

	double IniDouble(const wchar_t* key, double fallback)
	{
		wchar_t buf[64]{};
		GetPrivateProfileStringW(L"Ring", key, L"", buf, 64, IniPath().c_str());
		if (!buf[0])
			return fallback;
		wchar_t* end = nullptr;
		const double v = wcstod(buf, &end);
		return (end != buf) ? v : fallback;
	}

	unsigned IniUInt(const wchar_t* key, unsigned fallback)
	{
		wchar_t buf[64]{};
		GetPrivateProfileStringW(L"Ring", key, L"", buf, 64, IniPath().c_str());
		if (!buf[0])
			return fallback;
		wchar_t* end = nullptr;
		const unsigned long v = wcstoul(buf, &end, 0);
		return (end != buf) ? static_cast<unsigned>(v) : fallback;
	}

	Settings LoadSettings()
	{
		Settings s;
		s.centerXH = IniDouble(L"CenterXH", s.centerXH);
		s.bottomYH = IniDouble(L"BottomYH", s.bottomYH);
		s.legacyX = IniDouble(L"CenterX", s.legacyX);
		s.legacyY = IniDouble(L"CenterY", s.legacyY);
		s.radiusH = IniDouble(L"RadiusH", s.radiusH);
		s.thick1080 = IniDouble(L"Thickness1080", s.thick1080);
		s.ringArgb = IniUInt(L"RingARGB", s.ringArgb);
		s.trackArgb = IniUInt(L"TrackARGB", s.trackArgb);
		s.baseArgb = IniUInt(L"BaseARGB", s.baseArgb);
		s.globalAlpha = static_cast<std::uint8_t>(IniUInt(L"GlobalAlpha", s.globalAlpha) & 0xFF);
		s.enabled = IniUInt(L"Enabled", s.enabled ? 1 : 0) != 0;
		s.fightingOnly = IniUInt(L"FightingOnly", s.fightingOnly ? 1 : 0) != 0;
		s.holeFrac = IniDouble(L"HoleFrac", s.holeFrac);
		s.featherPx1080 = IniDouble(L"FeatherPx", s.featherPx1080);
		s.style = static_cast<int>(IniUInt(L"Style", static_cast<unsigned>(s.style)));
		s.autoAnchor = IniUInt(L"AutoAnchor", s.autoAnchor ? 1 : 0) != 0;
		s.anchorDXH = IniDouble(L"AnchorDXH", s.anchorDXH);
		s.anchorDYH = IniDouble(L"AnchorDYH", s.anchorDYH);
		s.radiusMul = IniDouble(L"RadiusMul", s.radiusMul);
		s.lowHPFrac = IniDouble(L"LowHPFrac", s.lowHPFrac);
		s.lowOuterArgb = IniUInt(L"LowOuterARGB", s.lowOuterArgb);
		s.lowInnerArgb = IniUInt(L"LowInnerARGB", s.lowInnerArgb);
		s.pulseHz = IniDouble(L"PulseHz", s.pulseHz);
		s.dmgGhostSec = IniDouble(L"DmgGhostSec", s.dmgGhostSec);
		s.ghostArgb = IniUInt(L"GhostARGB", s.ghostArgb);
		s.dmgPulseSec = IniDouble(L"DmgPulseSec", s.dmgPulseSec);
		s.dmgPulseArgb = IniUInt(L"DmgPulseARGB", s.dmgPulseArgb);
		s.healSec = IniDouble(L"HealSec", s.healSec);
		s.healArgb = IniUInt(L"HealARGB", s.healArgb);
		s.hubRingArgb = IniUInt(L"HubRingARGB", s.hubRingArgb);
		s.hubRing1080 = IniDouble(L"HubRing1080", s.hubRing1080);
		s.grooveCount = static_cast<int>(IniUInt(L"GrooveCount",
			static_cast<unsigned>(s.grooveCount)));
		s.groovePx1080 = IniDouble(L"GroovePx1080", s.groovePx1080);
		s.grooveOuterMul = IniDouble(L"GrooveOuterMul", s.grooveOuterMul);
		return s;
	}

	void WriteDefaults()
	{
		const std::wstring path = IniPath();
		if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return;
		const Settings d;
		std::ostringstream a, t;
		a << "0x" << std::hex << std::uppercase << d.ringArgb;
		t << "0x" << std::hex << std::uppercase << d.trackArgb;
		WritePrivateProfileStringW(L"Ring", L"CenterXH", std::to_wstring(d.centerXH).c_str(), path.c_str());
		WritePrivateProfileStringW(L"Ring", L"BottomYH", std::to_wstring(d.bottomYH).c_str(), path.c_str());
		WritePrivateProfileStringW(L"Ring", L"RadiusH", std::to_wstring(d.radiusH).c_str(), path.c_str());
		WritePrivateProfileStringW(L"Ring", L"Thickness1080", std::to_wstring(d.thick1080).c_str(), path.c_str());
		WritePrivateProfileStringW(L"Ring", L"RingARGB", std::wstring(a.str().begin(), a.str().end()).c_str(), path.c_str());
		WritePrivateProfileStringW(L"Ring", L"TrackARGB", std::wstring(t.str().begin(), t.str().end()).c_str(), path.c_str());
		WritePrivateProfileStringW(L"Ring", L"BaseARGB", L"0x0", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"GlobalAlpha", L"255", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"Enabled", L"1", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"FightingOnly", L"0", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"HoleFrac", std::to_wstring(d.holeFrac).c_str(), path.c_str());
		WritePrivateProfileStringW(L"Ring", L"FeatherPx", std::to_wstring(d.featherPx1080).c_str(), path.c_str());
		WritePrivateProfileStringW(L"Ring", L"Style", L"1", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"AutoAnchor", L"1", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"AnchorDXH", L"0", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"AnchorDYH", L"0", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"RadiusMul", L"1", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"LowHPFrac", L"0.25", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"LowOuterARGB", L"0xFFFF7A85", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"LowInnerARGB", L"0xFF8A1020", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"PulseHz", L"0.5", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"DmgGhostSec", L"1", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"GhostARGB", L"0x80E02030", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"DmgPulseSec", L"0.6", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"DmgPulseARGB", L"0x70FF2030", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"HealSec", L"0.7", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"HealARGB", L"0x9030E060", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"HubRingARGB", L"0xFFFFFFFF", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"HubRing1080", L"2", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"GrooveCount", L"5", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"GroovePx1080", L"1.5", path.c_str());
		WritePrivateProfileStringW(L"Ring", L"GrooveOuterMul", L"2.5", path.c_str());
	}

	void LogInfo(const std::string& m) { HJ::Logger::Info(std::string("[FRAME OVERLAY] ") + m); }
	void LogCStr(const char* m)
	{
		HJ::Logger::Info(std::string("[FRAME OVERLAY] ") + m);
	}
	void LogError(const std::string& m)
	{
		HJ::Logger::Error(std::string("[FRAME OVERLAY] ") + m);
	}

	// crash breadcrumbs so the SEH report names the dying stage.
	// t_lockHeld lets the handler drop the lock (SEH skips dtors).
	std::atomic<int> g_crashStage{ 0 };
	thread_local bool t_lockHeld = false;

	void Stage(int s) { g_crashStage.store(s, std::memory_order_relaxed); }

	const char* StageName(int s)
	{
		switch (s)
		{
		case 0: return "idle";
		case 10: return "present-enter";
		case 11: return "present-swapcheck";
		case 12: return "present-f11poll";
		case 13: return "present-gamestate";
		case 14: return "present-gate";
		case 20: return "execute-enter";
		case 21: return "execute-queuecheck";
		case 22: return "execute-device";
		case 23: return "execute-backbuffer";
		case 24: return "execute-record";
		case 25: return "execute-inject";
		default: return "unknown";
		}
	}

	// cached game-state reads: if the game tears stuff down mid-frame
	// (pause/title), keep last-known values and skip instead of dying.
	bool g_cachedFighting = false;
	bool g_cachedKnown = false;
	bool g_cachedReady = false;
	bool g_cachedAlive = false;
	bool g_cachedShown = false;
	float g_cachedRatio = 1.0f;
	bool g_stateWarned = false;

	void StateWarn(const char* what)
	{
		if (!g_stateWarned)
		{
			g_stateWarned = true;
			std::ostringstream m;
			m << "game-state read fault (" << what
				<< "); using cached values, frames skipped while torn down.";
			LogError(m.str());
		}
	}

	bool SafeFighting(bool& out)
	{
		__try
		{
			bool known = HJ::Engine::TryGetFightingState(out);
			g_cachedKnown = known;
			if (known)
				g_cachedFighting = out;
			return known;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out = g_cachedFighting;
			StateWarn("fighting");
			return g_cachedKnown;
		}
	}

	bool SafeBridgeReady()
	{
		__try { bool r = HJ::HUD::RadialHealthBridge::IsReady(); g_cachedReady = r; return r; }
		__except (EXCEPTION_EXECUTE_HANDLER) { StateWarn("bridge-ready"); return g_cachedReady; }
	}

	bool SafeSceneAlive()
	{
		__try { bool r = HJ::HUD::RadialDiscDrive::IsSceneAlive(); g_cachedAlive = r; return r; }
		__except (EXCEPTION_EXECUTE_HANDLER) { StateWarn("scene-alive"); return g_cachedAlive; }
	}

	bool SafeHudShown()
	{
		__try { bool r = HJ::HUD::RadialDiscDrive::IsHudShown(); g_cachedShown = r; return r; }
		__except (EXCEPTION_EXECUTE_HANDLER) { StateWarn("hud-shown"); return g_cachedShown; }
	}

	float SafeHealthRatio()
	{
		__try
		{
			float r = HJ::HUD::RadialHealthBridge::GetHealthRatio();
			g_cachedRatio = r;
			return r;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { StateWarn("health-ratio"); return g_cachedRatio; }
	}

	void SafeDumpSceneObjects()
	{
		__try { HJ::HUD::RadialDiscDrive::DumpSceneObjects(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { StateWarn("dump-scene"); }
	}

	void DropBackbuffers(); // defined below Barrier, used by EnsureBackbuffer

	SafetyHookInline g_presentHook{};
	SafetyHookInline g_executeHook{};
	SafetyHookInline g_resizeHook{};
	SafetyHookInline g_fullscreenHook{};
	SafetyHookInline g_targetHook{};
	SRWLOCK g_lock = SRWLOCK_INIT;

	std::atomic<bool> g_disabled{ false };

	inline void AcquireHeld()
	{
		AcquireSRWLockExclusive(&g_lock);
		t_lockHeld = true;
	}

	inline void ReleaseHeld()
	{
		t_lockHeld = false;
		ReleaseSRWLockExclusive(&g_lock);
	}

	// SEH report. runs as the exception filter so the hooks stay
	// pod-only (no /EHa needed). drops the lock, logs the stage,
	// kills the overlay, game lives.
	int ReportHookFault(const char* hook, DWORD code)
	{
		if (t_lockHeld)
		{
			t_lockHeld = false;
			ReleaseSRWLockExclusive(&g_lock);
		}
		std::ostringstream m;
		m << hook << " exception 0x" << std::hex << code
			<< " at stage " << StageName(g_crashStage.load())
			<< "; overlay disabled, game untouched.";
		LogError(m.str());
		g_disabled.store(true);
		Stage(0);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	IDXGISwapChain* g_swap = nullptr;
	ID3D12Device* g_device = nullptr;
	ID3D12DescriptorHeap* g_rtvHeap = nullptr;
	ID3D12CommandAllocator* g_alloc[kRecSlots]{};
	ID3D12GraphicsCommandList* g_lists[kRecSlots]{};
	struct RecKey
	{
		UINT buf = 0xFFFFFFFFu;
		UINT w = 0;
		UINT h = 0;
		int aspectQ = -1;
		int bucket = -1;
		int phaseQ = -1;
		std::uint64_t settingsRev = 0;
		bool valid = false;
	};
	RecKey g_keys[kRecSlots];
	std::uint64_t g_slotSerial[kRecSlots]{}; // last present this slot served
	std::uint64_t g_settingsRev = 0;
	ID3D12RootSignature* g_rootSig = nullptr;
	ID3D12PipelineState* g_pso = nullptr;
	ID3DBlob* g_vsBlob = nullptr;
	ID3DBlob* g_psBlob = nullptr;
	DXGI_FORMAT g_psoFmt = DXGI_FORMAT_UNKNOWN;
	ID3D12Resource* g_vb = nullptr;
	std::uint8_t* g_vbCpu = nullptr;
	ID3D12Resource* g_bufRes[kMaxBuffers]{};
	D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[kMaxBuffers]{};
	UINT g_bufW = 0, g_bufH = 0;
	// present can stretch the backbuffer to the window when their aspects
	// differ. >1 widens the disc in x so it lands round after the stretch.
	float g_aspectComp = 1.0f;
	DXGI_FORMAT g_bufFmt = DXGI_FORMAT_UNKNOWN;
	HWND g_bufWindow = nullptr;
	int g_aspectQ = 0;

	std::uint64_t g_presentSerial = 0;
	bool g_seenFighting = false;
	bool g_loggedInject = false;
	bool g_lowLatched = false;
	float g_trackRatio = 1.0f; // last seen hp, for damage/heal triggers
	float g_ghostFrom = 1.0f; // pre-hit value the red ghost drains from
	ULONGLONG g_dmgT0 = 0;
	ULONGLONG g_healT0 = 0;
	float g_pendingRatio = 1.0f;
	Settings g_pendingSettings;
	bool g_pendingValid = false;
	Settings g_liveSettings;
	bool g_settingsLoaded = false;
	DWORD g_lastF11Tick = 0;
	bool g_lastF11 = false;
	bool g_lastF12 = false;

	struct Vertex
	{
		float x, y, z;
		float r, g, b, a;
	};

	void ArgbToFloat(std::uint32_t argb, float alphaScale, float& r, float& g, float& b, float& a)
	{
		a = ((argb >> 24) & 0xFF) / 255.0f * alphaScale;
		r = ((argb >> 16) & 0xFF) / 255.0f;
		g = ((argb >> 8) & 0xFF) / 255.0f;
		b = (argb & 0xFF) / 255.0f;
	}

	// pie/ring slice as triangles. angles in radians, y down so up is clockwise.
	// rim verts take the outer color, hub/center verts the inner one. pass
	// the same color twice for a flat fill.
	int EmitSector(Vertex* out, int capacity, float cx, float cy,
		float rOuter, float rInner, double a0, double sweep,
		float cr, float cg, float cb, float ca,
		float crI, float cgI, float cbI, float caI)
	{
		if (rOuter <= 0.0f || sweep <= 0.0)
			return 0;
		int segs = static_cast<int>(72.0 * sweep / (2.0 * kPi)) + 1;
		if (segs < 1) segs = 1;
		if (segs > 120) segs = 120;
		int n = 0;
		auto rim = [&](double a, float r, float& x, float& y) {
			x = cx + r * static_cast<float>(std::cos(a));
			y = cy + r * static_cast<float>(std::sin(a));
		};
		for (int i = 0; i < segs; i++)
		{
			const double aA = a0 + sweep * i / segs;
			const double aB = a0 + sweep * (i + 1) / segs;
			float x0, y0, x1, y1, xi0, yi0, xi1, yi1;
			rim(aA, rOuter, x0, y0);
			rim(aB, rOuter, x1, y1);
			if (rInner <= 0.0f)
			{
				if (n + 3 > capacity) break;
				out[n++] = { cx, cy, 0.5f, crI, cgI, cbI, caI };
				out[n++] = { x0, y0, 0.5f, cr, cg, cb, ca };
				out[n++] = { x1, y1, 0.5f, cr, cg, cb, ca };
			}
			else
			{
				if (n + 6 > capacity) break;
				rim(aA, rInner, xi0, yi0);
				rim(aB, rInner, xi1, yi1);
				out[n++] = { x0, y0, 0.5f, cr, cg, cb, ca };
				out[n++] = { x1, y1, 0.5f, cr, cg, cb, ca };
				out[n++] = { xi0, yi0, 0.5f, crI, cgI, cbI, caI };
				out[n++] = { x1, y1, 0.5f, cr, cg, cb, ca };
				out[n++] = { xi1, yi1, 0.5f, crI, cgI, cbI, caI };
				out[n++] = { xi0, yi0, 0.5f, crI, cgI, cbI, caI };
			}
		}
		return n;
	}

	// thin ring outline over an angle range. density follows the sweep:
	// a 2px line doesnt need 64 segs for a sliver.
	int EmitRingSeg(Vertex* out, int capacity, float cx, float cy,
		float rMid, float halfW, double a0, double sweep,
		float cr, float cg, float cb, float ca)
	{
			if (rMid <= 0.0f || halfW <= 0.0f || ca <= 0.0f || sweep <= 0.0)
				return 0;
			int segs = static_cast<int>(64.0 * sweep / (2.0 * kPi));
			if (segs < 1) segs = 1;
			if (segs > 64) segs = 64;
			const float rO = rMid + halfW;
			const float rI = (rMid > halfW) ? rMid - halfW : 0.0f;
			int n = 0;
			for (int i = 0; i < segs; i++)
			{
				const double aA = a0 + sweep * i / segs;
				const double aB = a0 + sweep * (i + 1) / segs;
				const float xo0 = cx + rO * static_cast<float>(std::cos(aA));
				const float yo0 = cy + rO * static_cast<float>(std::sin(aA));
				const float xo1 = cx + rO * static_cast<float>(std::cos(aB));
				const float yo1 = cy + rO * static_cast<float>(std::sin(aB));
				const float xi0 = cx + rI * static_cast<float>(std::cos(aA));
				const float yi0 = cy + rI * static_cast<float>(std::sin(aA));
				const float xi1 = cx + rI * static_cast<float>(std::cos(aB));
				const float yi1 = cy + rI * static_cast<float>(std::sin(aB));
				if (n + 6 > capacity) break;
				out[n++] = { xo0, yo0, 0.5f, cr, cg, cb, ca };
				out[n++] = { xo1, yo1, 0.5f, cr, cg, cb, ca };
				out[n++] = { xi0, yi0, 0.5f, cr, cg, cb, ca };
				out[n++] = { xo1, yo1, 0.5f, cr, cg, cb, ca };
				out[n++] = { xi1, yi1, 0.5f, cr, cg, cb, ca };
				out[n++] = { xi0, yi0, 0.5f, cr, cg, cb, ca };
			}
			return n;
		}

	const char* kHlsl =
		"struct VIn { float3 p : POSITION; float4 c : COLOR; };"
		"struct VOut { float4 p : SV_POSITION; float4 c : COLOR; };"
		"VOut VS(VIn i) { VOut o; o.p = float4(i.p, 1.0); o.c = i.c; return o; }"
		"float4 PS(VOut i) : SV_TARGET { return i.c; }";

	bool EnsureDevice(IDXGISwapChain* swap)
	{
		if (g_device)
			return true;
		if (FAILED(swap->GetDevice(IID_PPV_ARGS(&g_device))) || !g_device)
		{
			g_device = nullptr;
			return false;
		}

		ID3DBlob* vsBlob = nullptr;
		ID3DBlob* psBlob = nullptr;
		ID3DBlob* errBlob = nullptr;
		HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr,
			nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, &errBlob);
		if (errBlob) errBlob->Release();
		if (FAILED(hr) || !vsBlob)
		{
			LogInfo("D3DCompile VS failed; overlay disabled.");
			if (vsBlob) vsBlob->Release();
			g_device->Release();
			g_device = nullptr;
			g_disabled.store(true);
			return false;
		}
		hr = D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr,
			nullptr, "PS", "ps_5_0", 0, 0, &psBlob, &errBlob);
		if (errBlob) errBlob->Release();
		if (FAILED(hr) || !psBlob)
		{
			LogInfo("D3DCompile PS failed; overlay disabled.");
			vsBlob->Release();
			if (psBlob) psBlob->Release();
			g_device->Release();
			g_device = nullptr;
			g_disabled.store(true);
			return false;
		}

		D3D12_ROOT_SIGNATURE_DESC rsDesc{};
		rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		ID3DBlob* rsBlob = nullptr;
		hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			&rsBlob, &errBlob);
		if (errBlob) errBlob->Release();
		if (FAILED(hr))
		{
			LogInfo("root signature serialize failed; overlay disabled.");
			vsBlob->Release();
			psBlob->Release();
			if (rsBlob) rsBlob->Release();
			g_device->Release();
			g_device = nullptr;
			g_disabled.store(true);
			return false;
		}
		hr = g_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
			rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
		rsBlob->Release();
		if (FAILED(hr))
		{
			LogInfo("root signature create failed; overlay disabled.");
			vsBlob->Release();
			psBlob->Release();
			g_device->Release();
			g_device = nullptr;
			g_disabled.store(true);
			return false;
		}

		D3D12_INPUT_ELEMENT_DESC inputDesc[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		// pso needs the backbuffer format so this waits for it.
		// blobs are tiny, just leak em.
		g_vsBlob = vsBlob;
		g_psBlob = psBlob;

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDesc.NumDescriptors = kMaxBuffers;
		if (FAILED(g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_rtvHeap))))
		{
			LogInfo("RTV heap failed; overlay disabled.");
			g_rootSig->Release(); g_rootSig = nullptr;
			g_device->Release(); g_device = nullptr;
			g_disabled.store(true);
			return false;
		}
		const UINT inc = g_device->GetDescriptorHandleIncrementSize(
			D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		for (int i = 0; i < kMaxBuffers; i++)
		{
			g_rtv[i] = h;
			h.ptr += inc;
		}
		for (int i = 0; i < kRecSlots; i++)
		{
			if (FAILED(g_device->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]))))
			{
				LogInfo("allocator failed; overlay disabled.");
				g_disabled.store(true);
				return false;
			}
		}

		D3D12_HEAP_PROPERTIES uploadHeap{};
		uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC vbDesc{};
		vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		vbDesc.Width = kMaxVbVerts * sizeof(Vertex);
		vbDesc.Height = 1;
		vbDesc.DepthOrArraySize = 1;
		vbDesc.MipLevels = 1;
		vbDesc.SampleDesc.Count = 1;
		vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(g_device->CreateCommittedResource(&uploadHeap,
			D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr, IID_PPV_ARGS(&g_vb))))
		{
			LogInfo("vertex buffer failed; overlay disabled.");
			g_disabled.store(true);
			return false;
		}
		D3D12_RANGE noRead{ 0, 0 };
		if (FAILED(g_vb->Map(0, &noRead,
			reinterpret_cast<void**>(&g_vbCpu))) || !g_vbCpu)
		{
			LogInfo("vertex map failed; overlay disabled.");
			g_disabled.store(true);
			return false;
		}

		LogInfo("device resources ready.");
		return true;
	}

	bool CreatePipeline(DXGI_FORMAT fmt)
	{
		if (g_pso && g_psoFmt == fmt)
			return true;
		for (int i = 0; i < kRecSlots; i++)
		{
			if (g_lists[i]) { g_lists[i]->Release(); g_lists[i] = nullptr; }
			g_keys[i].valid = false;
		}
		if (g_pso) { g_pso->Release(); g_pso = nullptr; }
		g_psoFmt = DXGI_FORMAT_UNKNOWN;
		if (!g_vsBlob || !g_psBlob || !g_device || !g_rootSig)
			return false;

		D3D12_INPUT_ELEMENT_DESC inputDesc[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		D3D12_BLEND_DESC blend{};
		blend.RenderTarget[0].BlendEnable = TRUE;
		blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
		blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		D3D12_RASTERIZER_DESC rast{};
		rast.FillMode = D3D12_FILL_MODE_SOLID;
		rast.CullMode = D3D12_CULL_MODE_NONE;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = g_rootSig;
		psoDesc.VS = { g_vsBlob->GetBufferPointer(), g_vsBlob->GetBufferSize() };
		psoDesc.PS = { g_psBlob->GetBufferPointer(), g_psBlob->GetBufferSize() };
		psoDesc.BlendState = blend;
		psoDesc.InputLayout = { inputDesc, 2 };
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.RasterizerState = rast;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = fmt;
		psoDesc.SampleDesc.Count = 1;
		if (FAILED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pso))))
		{
			g_pso = nullptr;
			return false;
		}
		g_psoFmt = fmt;
		return true;
	}

	bool EnsureBackbuffer(UINT index)
	{
		if (index >= (UINT)kMaxBuffers || !g_device || !g_swap)
			return false;
		DXGI_SWAP_CHAIN_DESC desc{};
		g_swap->GetDesc(&desc);
		static bool loggedBufCount = false;
		if (!loggedBufCount)
		{
			loggedBufCount = true;
			if (desc.BufferCount > (UINT)kMaxBuffers)
			{
				std::ostringstream m;
				m << "swapchain BufferCount=" << desc.BufferCount
					<< " exceeds tracked " << kMaxBuffers
					<< ". disc may skip on high indices.";
				LogError(m.str());
			}
		}
		// stretch measure, quantized so float wobble doesnt bust the cache.
		float comp = 1.0f;
		if (desc.OutputWindow)
		{
			RECT rc{};
			if (GetClientRect(desc.OutputWindow, &rc) &&
				rc.right > 0 && rc.bottom > 0 &&
				desc.BufferDesc.Width > 0 && desc.BufferDesc.Height > 0)
			{
				const double clientA = (double)rc.right / (double)rc.bottom;
				const double bufA = (double)desc.BufferDesc.Width /
					(double)desc.BufferDesc.Height;
				if (clientA > 0.0 && bufA > 0.0)
					comp = (float)(bufA / clientA);
			}
		}
		if (!(comp > 0.5f && comp < 2.0f))
			comp = 1.0f;
		g_aspectComp = comp;
		const int aspectQ = (int)(comp * 1024.0f);
		const bool geomChanged = desc.BufferDesc.Width != g_bufW ||
			desc.BufferDesc.Height != g_bufH ||
			desc.BufferDesc.Format != g_bufFmt ||
			desc.OutputWindow != g_bufWindow ||
			aspectQ != g_aspectQ;
		if (geomChanged)
		{
			g_bufW = desc.BufferDesc.Width;
			g_bufH = desc.BufferDesc.Height;
			g_bufFmt = desc.BufferDesc.Format;
			g_bufWindow = desc.OutputWindow;
			g_aspectQ = aspectQ;
			DropBackbuffers();
			// stale geometry here is the squished-disc bug. the drop above
			// wipes the cache so a resize re-records instead of waiting
			// for new damage.
		}
		// one-shot so blur reports come with numbers: if the backbuffer is
		// smaller than the window, present upscales and everything (disc
		// included) goes soft. nothing to fix on our side then.
		static bool loggedSurface = false;
		if (!loggedSurface)
		{
			loggedSurface = true;
			int cw = 0, ch = 0;
			if (desc.OutputWindow)
			{
				RECT rc{};
				if (GetClientRect(desc.OutputWindow, &rc)) { cw = rc.right; ch = rc.bottom; }
			}
			std::ostringstream m;
			m << "surface " << desc.BufferDesc.Width << "x" << desc.BufferDesc.Height
				<< " fmt " << static_cast<int>(desc.BufferDesc.Format)
				<< " client " << cw << "x" << ch
				<< " aspectComp " << comp;
			LogInfo(m.str());
		}
		if (!g_bufRes[index])
		{
			if (FAILED(g_swap->GetBuffer(index, IID_PPV_ARGS(&g_bufRes[index]))))
				return false;
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = desc.BufferDesc.Format;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			g_device->CreateRenderTargetView(g_bufRes[index], &rtvDesc, g_rtv[index]);
		}
		if ((!g_pso || g_psoFmt != desc.BufferDesc.Format) &&
			!CreatePipeline(desc.BufferDesc.Format))
		{
			LogInfo("pipeline failed; overlay disabled.");
			g_disabled.store(true);
			return false;
		}
		return g_bufRes[index] != nullptr;
	}

	void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter = after;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &b);
	}

	// drop our refs to the swapchain buffers. dxgi transitions (resize,
	// fullscreen flips) can fail or freeze with our refs outstanding, so
	// this runs before them. lock must be held.
	void DropBackbuffers()
	{
		for (int i = 0; i < kMaxBuffers; i++)
		{
			if (g_bufRes[i]) { g_bufRes[i]->Release(); g_bufRes[i] = nullptr; }
		}
		for (int i = 0; i < kRecSlots; i++)
			g_keys[i].valid = false;
	}

	// records into a cache slot. slots only get reused for the same key,
	// or after sitting out a few presents so the gpu is done with em.
	// pulsePhase < 0 = steady fill; else 0..1 white->red blend amount.
	// ghost = trailing red edge (decays to ratio), dmgA/healA = full-face
	// wash amounts, all 0 when idle.
	bool RecordOverlay(UINT index, int slot, float ratio, float pulsePhase,
		float ghost, float dmgA, float healA,
		const Settings& s, UINT w, UINT h)
	{
		const float alphaScale = s.globalAlpha / 255.0f;
		float br, bg, bb, ba;
		ArgbToFloat(s.baseArgb, alphaScale, br, bg, bb, ba);
		float rr, rg, rb, ra;
		ArgbToFloat(s.ringArgb, alphaScale, rr, rg, rb, ra);
		float tr, tg, tb, ta;
		ArgbToFloat(s.trackArgb, alphaScale, tr, tg, tb, ta);
		float gr, gg, gb, ga;
		ArgbToFloat(s.ghostArgb, alphaScale, gr, gg, gb, ga);
		// low hp: rim light red, hub dark red, blended toward the normal
		// white by the pulse so it breathes white<->red.
		float rimR = rr, rimG = rg, rimB = rb, rimA = ra;
		float hubR = rr, hubG = rg, hubB = rb, hubA = ra;
		if (pulsePhase >= 0.0f)
		{
			float lr, lg, lb, la;
			ArgbToFloat(s.lowOuterArgb, alphaScale, lr, lg, lb, la);
			float dr, dg, db, da;
			ArgbToFloat(s.lowInnerArgb, alphaScale, dr, dg, db, da);
			const float k = 0.35f + 0.65f * pulsePhase;
			rimR = rr + (lr - rr) * k;
			rimG = rg + (lg - rg) * k;
			rimB = rb + (lb - rb) * k;
			rimA = ra + (la - ra) * k;
			hubR = rr + (dr - rr) * k;
			hubG = rg + (dg - rg) * k;
			hubB = rb + (db - rb) * k;
			hubA = ra + (da - ra) * k;
		}

		const bool drawBase = ba > 0.003f;
		const bool drawWhite = ratio > 0.001f && ra > 0.003f;
		const bool drawGhost = ghost > ratio + 0.0005f && ga > 0.003f;
		const bool drawSector = drawWhite || drawGhost;
		const bool drawTrack = ta > 0.003f && s.thick1080 != 0.0;
		const bool drawWash = dmgA > 0.003f || healA > 0.003f;
		if (!drawBase && !drawSector && !drawTrack && !drawWash)
			return false;

		double cx, cy;
		float r;
		if (s.autoAnchor)
		{
			// locked anchor + nudge. no per-res fiddling.
			cx = (kAutoCX + s.anchorDXH) * h;
			cy = h - (kAutoBY + s.anchorDYH) * h;
			r = static_cast<float>(kAutoR * s.radiusMul * h);
		}
		else
		{
			cx = (s.legacyX >= 0.0) ? s.legacyX * w : s.centerXH * h;
			cy = (s.legacyY >= 0.0) ? s.legacyY * h : h - s.bottomYH * h;
			r = static_cast<float>(s.radiusH * h);
		}
		float penW = static_cast<float>(s.thick1080 * h / 1080.0);
		if (penW < 0.0f) penW = 0.0f;
		if (r <= 0.0f)
			return false;
		double hole = s.holeFrac;
		if (hole < 0.0) hole = 0.0;
		if (hole > 0.9) hole = 0.9;

		// Screen -> NDC. kept cause the flip is easy to get backwards.
		const float fx = 2.0f / w, fy = 2.0f / h;
		Vertex* v = reinterpret_cast<Vertex*>(g_vbCpu) + slot * kSlotVerts;
		int n = 0;
		auto xform = [&](float sx, float sy, float& dx, float& dy) {
			// pre-squash x about the center so present's aspect stretch
			// lands it round. no-op when aspects already match.
			sx = (float)cx + (sx - (float)cx) * g_aspectComp;
			dx = sx * fx - 1.0f;
			dy = 1.0f - sy * fy;
		};
		Vertex tmp[kSlotVerts];
		int m = 0;
		const double a0 = -kPi / 2.0;
		if (drawBase)
			m += EmitSector(tmp + m, kSlotVerts - m, (float)cx, (float)cy, r, 0.0f,
				0.0, 2.0 * kPi, br, bg, bb, ba, br, bg, bb, ba);
		if (drawTrack && penW > 0.0f)
			m += EmitSector(tmp + m, kSlotVerts - m, (float)cx, (float)cy, r,
				r - penW, 0.0, 2.0 * kPi, tr, tg, tb, ta, tr, tg, tb, ta);
		// groove pulse: rings overlaying the scene grooves in the band
		// OUTSIDE the ivory disc (white edge -> near the rim). never
		// inside it. damage red, heal green, both die out fast.
		auto grooveRings = [&](float pr, float pg, float pb, float pa) {
			if (pa <= 0.003f) return;
			double mul = s.grooveOuterMul;
			if (!(mul > 1.2)) mul = 1.2;
			if (mul > 4.0) mul = 4.0;
			int count = s.grooveCount;
			if (count < 1) count = 1;
			if (count > 8) count = 8;
			float halfW = (float)(s.groovePx1080 * h / 1080.0) * 0.5f;
			if (halfW < 0.5f) halfW = 0.5f;
			const float lo = r * 1.04f; // clear of the ivory edge
			const float hi = r * (float)mul;
			for (int i = 0; i < count; i++)
			{
				const float f = (float)(i + 1) / (float)(count + 1);
				const float rMid = lo + (hi - lo) * f;
				m += EmitRingSeg(tmp + m, kSlotVerts - m, (float)cx, (float)cy,
					rMid, halfW, 0.0, 2.0 * kPi, pr, pg, pb, pa);
			}
		};
		if (dmgA > 0.003f)
		{
			float pr, pg, pb, pa;
			ArgbToFloat(s.dmgPulseArgb, alphaScale, pr, pg, pb, pa);
			grooveRings(pr, pg, pb, pa * dmgA);
		}
		if (healA > 0.003f)
		{
			float hr, hg, hb, ha;
			ArgbToFloat(s.healArgb, alphaScale, hr, hg, hb, ha);
			grooveRings(hr, hg, hb, ha * healA * healA);
		}
		if (drawSector)
		{
			float inner = 0.0f;
			if (s.style == 1 && hole > 0.001)
				inner = r * (float)hole;
			if (s.style == 0)
				inner = (penW > 0.0f) ? r - penW : 0.0f;
			// faint outer band under a smaller solid core. cheap fake blur.
			float feather = (float)(s.featherPx1080 * h / 1080.0);
			if (feather < 0.0f) feather = 0.0f;
			if (drawWhite)
			{
				if (s.style == 1 && feather > 0.5f)
				{
					float bandIn = r - feather;
					if (bandIn < inner) bandIn = inner;
					if (bandIn < r)
						m += EmitSector(tmp + m, kSlotVerts - m, (float)cx, (float)cy, r,
							bandIn, a0, 2.0 * kPi * ratio, rimR, rimG, rimB, rimA * 0.35f,
							hubR, hubG, hubB, hubA * 0.35f);
					if (r - feather > inner)
						m += EmitSector(tmp + m, kSlotVerts - m, (float)cx, (float)cy, r - feather,
							inner, a0, 2.0 * kPi * ratio, rimR, rimG, rimB, rimA,
							hubR, hubG, hubB, hubA);
				}
				else
				{
					m += EmitSector(tmp + m, kSlotVerts - m, (float)cx, (float)cy, r,
						inner, a0, 2.0 * kPi * ratio, rimR, rimG, rimB, rimA,
						hubR, hubG, hubB, hubA);
				}
			}
			// trailing red of recently lost hp. flat, no feather.
			if (drawGhost)
			{
				const double g0 = a0 + 2.0 * kPi * ratio;
				m += EmitSector(tmp + m, kSlotVerts - m, (float)cx, (float)cy, r,
					inner, g0, 2.0 * kPi * (ghost - ratio),
					gr, gg, gb, ga, gr, gg, gb, ga);
			}
			// center white ring around the hub emblem.
			float ringW = (float)(s.hubRing1080 * h / 1080.0);
			if (s.style == 1 && hole > 0.001 && ringW > 0.5f)
			{
				float wr, wg, wb, wa;
				ArgbToFloat(s.hubRingArgb, alphaScale, wr, wg, wb, wa);
				if (wa > 0.003f)
				{
					m += EmitSector(tmp + m, kSlotVerts - m, (float)cx, (float)cy,
						inner + ringW, inner, 0.0, 2.0 * kPi,
						wr, wg, wb, wa, wr, wg, wb, wa);
				}
			}
		}
		for (int i = 0; i < m && n < kSlotVerts; i++, n++)
		{
			float dx, dy;
			xform(tmp[i].x, tmp[i].y, dx, dy);
			v[n] = { dx, dy, 0.5f, tmp[i].r, tmp[i].g, tmp[i].b, tmp[i].a };
		}
		if (n == 0)
			return false;

		ID3D12GraphicsCommandList* list = g_lists[slot];
		if (!list)
		{
			if (FAILED(g_device->CreateCommandList(0,
				D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[slot], g_pso,
				IID_PPV_ARGS(&list))))
				return false;
			list->Close(); // created open; close so the Reset below is legal
			g_lists[slot] = list;
		}
		if (FAILED(g_alloc[slot]->Reset()))
			return false;
		if (FAILED(list->Reset(g_alloc[slot], g_pso)))
			return false;

		Barrier(list, g_bufRes[index],
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		list->OMSetRenderTargets(1, &g_rtv[index], FALSE, nullptr);
		D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
		list->RSSetViewports(1, &vp);
		D3D12_RECT sc{ 0, 0, (LONG)w, (LONG)h };
		list->RSSetScissorRects(1, &sc);
		D3D12_VERTEX_BUFFER_VIEW vbv{};
		vbv.BufferLocation = g_vb->GetGPUVirtualAddress() +
			slot * (UINT)(kSlotVerts * sizeof(Vertex));
		vbv.SizeInBytes = (UINT)(n * sizeof(Vertex));
		vbv.StrideInBytes = sizeof(Vertex);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->IASetVertexBuffers(0, 1, &vbv);
		list->DrawInstanced((UINT)n, 1, 0, 0);
		Barrier(list, g_bufRes[index],
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		if (FAILED(list->Close()))
			return false;
		return true;
	}

	void PresentHookBody(IDXGISwapChain* swap)
	{
		Stage(10);
		AcquireHeld();
		if (g_swap != swap)
		{
			Stage(11);
			if (g_device) { /* keep: same device usually */ }
			g_swap = swap;
			// new swapchain, old buffers are garbage. drop em or the
			// disc draws into a dead surface.
			DropBackbuffers();
			if (!g_settingsLoaded)
			{
				WriteDefaults();
				g_liveSettings = LoadSettings();
				g_settingsLoaded = true;
				g_settingsRev++;
			}
		}
		Stage(12);
		const DWORD now = GetTickCount();
		if (now - g_lastF11Tick > 500)
		{
			g_lastF11Tick = now;
			const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
			if (f11 && !g_lastF11)
			{
				g_liveSettings = LoadSettings();
				g_settingsRev++;
				LogInfo("settings reloaded (F11).");
			}
			g_lastF11 = f11;
			// F12: hud object snapshot for the ui-gate hunt. take one
			// per ui state (hud up, phone, pause, title) and diff.
			const bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
			if (f12 && !g_lastF12)
			{
				bool st = false;
				const bool known = SafeFighting(st);
				std::ostringstream m;
				m << "snapshot ready="
					<< SafeBridgeReady()
					<< " fightingKnown=" << known
					<< " fighting=" << st
					<< " sceneAlive="
					<< SafeSceneAlive()
					<< " hudShown="
					<< SafeHudShown();
				LogInfo(m.str());
				SafeDumpSceneObjects();
			}
			g_lastF12 = f12;
		}
		// latch the real engine flag, not the gated one. hud shows on
		// fight start even before the first hp update, so latching
		// inside the gate below would deadlock it.
		// every read below is SEH-guarded + cached: pause/title teardown
		// must skip frames, never kill Present.
		Stage(13);
		bool engineFighting = false;
		const bool engineKnown = SafeFighting(engineFighting);
		if (engineKnown && engineFighting)
			g_seenFighting = true;
		const bool fightingGate = !g_liveSettings.fightingOnly ||
			!engineKnown || engineFighting;
		Stage(14);
		if (g_liveSettings.enabled &&
			(SafeBridgeReady() || g_seenFighting) &&
			fightingGate &&
			SafeSceneAlive() &&
			SafeHudShown())
		{
			const float trueRatio = std::clamp(
				SafeHealthRatio(),
				0.0f, 1.0f);
				// damage/heal triggers for the ghost + wash anims. white
				// snaps, red trails from the pre-hit value.
				const ULONGLONG nowMs = GetTickCount64();
				const float eps = 0.0001f;
				if (trueRatio < g_trackRatio - eps)
				{
					g_ghostFrom = g_trackRatio;
					g_dmgT0 = nowMs;
				}
				else if (trueRatio > g_trackRatio + eps)
				{
					g_healT0 = nowMs;
				}
				g_trackRatio = trueRatio;
				g_pendingRatio = trueRatio;
				g_pendingSettings = g_liveSettings;
				g_pendingValid = true;
				g_presentSerial++;
			}
			else
			{
				g_pendingValid = false;
			}
			ReleaseHeld();
		Stage(0);
	}

	HRESULT WINAPI PresentHook(IDXGISwapChain* swap, UINT sync, UINT flags)
	{
		if (!g_disabled.load())
		__try
		{
			PresentHookBody(swap);
		}
		__except (ReportHookFault("PresentHook", GetExceptionCode()))
		{
		}
		return g_presentHook.call<HRESULT>(swap, sync, flags);
	}

	void WINAPI ExecuteHook(ID3D12CommandQueue* queue, UINT count,
		ID3D12CommandList* const* lists)
	{
		bool injected = false;
		if (!g_disabled.load() && queue && count < 60)
		__try
		{
			Stage(20);
			D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
			if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
			{
				Stage(21);
				AcquireHeld();
				// late game passes paint over an early injection so this goes
				// in every submit. steady hp just reuses the cached list.
				if (g_pendingValid && g_swap && EnsureDevice(g_swap))
				{
					Stage(22);
					UINT idx = 0;
					IDXGISwapChain3* swap3 = nullptr;
					if (SUCCEEDED(g_swap->QueryInterface(
						IID_PPV_ARGS(&swap3))) && swap3)
					{
						idx = swap3->GetCurrentBackBufferIndex();
						swap3->Release();
					}
					ID3D12GraphicsCommandList* hitList = nullptr;
					Stage(23);
					if (EnsureBackbuffer(idx))
					{
						Stage(24);
						int bucket = (int)(g_pendingRatio * 2048.0f);
						// low hp latch with a little hysteresis so the rim
						// doesnt flicker at the boundary.
						const double frac = g_pendingSettings.lowHPFrac;
						const double hz = g_pendingSettings.pulseHz;
						if (frac > 0.0 && g_pendingRatio < (float)frac)
							g_lowLatched = true;
						else if (frac <= 0.0 || g_pendingRatio > (float)(frac + 0.02))
							g_lowLatched = false;
						float pulsePhase = -1.0f;
						int phaseQ = 0;
						if (g_lowLatched && hz > 0.0)
						{
							const double t = GetTickCount64() / 1000.0;
							pulsePhase = 0.5f + 0.5f * static_cast<float>(
								std::sin(2.0 * kPi * hz * t));
							phaseQ = static_cast<int>(pulsePhase * 15.999f);
						}
						// hit/heal anims decay off their trigger timestamps.
						// anything live forces a fresh record per frame (key
						// carries the frame serial, so submits within one
						// frame still share the list).
						const ULONGLONG nowMs = GetTickCount64();
						const float trueRatio = g_pendingRatio;
						float ghost = trueRatio;
						float dmgA = 0.0f, healA = 0.0f;
						const double gs = g_pendingSettings.dmgGhostSec;
						if (gs > 0.0 && nowMs >= g_dmgT0)
						{
							const double el = (nowMs - g_dmgT0) / 1000.0;
							if (el < gs && g_ghostFrom > trueRatio)
								ghost = trueRatio + (g_ghostFrom - trueRatio) *
									static_cast<float>(1.0 - el / gs);
						}
						const double ps = g_pendingSettings.dmgPulseSec;
						if (ps > 0.0 && nowMs >= g_dmgT0)
						{
							const double el = (nowMs - g_dmgT0) / 1000.0;
							if (el < ps) dmgA = static_cast<float>(1.0 - el / ps);
						}
						const double hs = g_pendingSettings.healSec;
						if (hs > 0.0 && nowMs >= g_healT0)
						{
							const double el = (nowMs - g_healT0) / 1000.0;
							if (el < hs) healA = static_cast<float>(1.0 - el / hs);
						}
						if (ghost > trueRatio + 0.0005f ||
							dmgA > 0.003f || healA > 0.003f)
							phaseQ = static_cast<int>((g_presentSerial & 63) + 128);
						int slot = -1;
						for (int i = 0; i < kRecSlots; i++)
						{
							if (g_keys[i].valid && g_keys[i].buf == idx &&
								g_keys[i].w == g_bufW && g_keys[i].h == g_bufH &&
								g_keys[i].aspectQ == g_aspectQ &&
								g_keys[i].bucket == bucket &&
								g_keys[i].phaseQ == phaseQ &&
								g_keys[i].settingsRev == g_settingsRev)
							{
								slot = i;
								break;
							}
						}
						if (slot < 0)
						{
							int oldest = 0;
							for (int i = 0; i < kRecSlots; i++)
							{
								if (!g_keys[i].valid)
								{
									oldest = i;
									break;
								}
								if (g_slotSerial[i] < g_slotSerial[oldest])
									oldest = i;
							}
							if (!g_keys[oldest].valid ||
								g_slotSerial[oldest] + kSlotMinAge <= g_presentSerial)
							{
								slot = oldest;
								if (RecordOverlay(idx, slot, g_pendingRatio,
									pulsePhase, ghost, dmgA, healA,
									g_pendingSettings, g_bufW, g_bufH))
								{
									g_keys[slot] = { idx, g_bufW, g_bufH, g_aspectQ,
										bucket, phaseQ, g_settingsRev, true };
								}
								else
								{
									slot = -1;
								}
							}
						}
						if (slot >= 0 && g_lists[slot])
						{
							g_slotSerial[slot] = g_presentSerial;
							hitList = g_lists[slot];
						}
					}
					if (hitList)
					{
						Stage(25);
						ID3D12CommandList* merged[65];
						for (UINT i = 0; i < count; i++)
							merged[i] = const_cast<ID3D12CommandList*>(lists[i]);
						merged[count] = hitList;
						ReleaseHeld();
						g_executeHook.call<void>(queue, count + 1, merged);
						injected = true;
						if (!g_loggedInject)
						{
							g_loggedInject = true;
							LogCStr("first injection ok.");
						}
					}
					else
					{
						ReleaseHeld();
					}
				}
				else
				{
					ReleaseHeld();
				}
			}
			Stage(0);
		}
		__except (ReportHookFault("ExecuteHook", GetExceptionCode()))
		{
		}
		if (!injected)
			g_executeHook.call<void>(queue, count, lists);
	}

	HRESULT WINAPI ResizeHook(IDXGISwapChain* swap, UINT count, UINT w,
		UINT h, DXGI_FORMAT fmt, UINT flags)
	{
		// release before dxgi acts, or the transition can fail with our
		// refs outstanding (thats the frozen fullscreen flip).
		AcquireSRWLockExclusive(&g_lock);
		DropBackbuffers();
		ReleaseSRWLockExclusive(&g_lock);
		LogInfo("ResizeBuffers; buffers dropped.");
		return g_resizeHook.call<HRESULT>(swap, count, w, h, fmt, flags);
	}

	HRESULT WINAPI FullscreenHook(IDXGISwapChain* swap, BOOL full,
		IDXGIOutput* out)
	{
		AcquireSRWLockExclusive(&g_lock);
		DropBackbuffers();
		ReleaseSRWLockExclusive(&g_lock);
		LogInfo(full ? "entering fullscreen; buffers dropped."
			: "leaving fullscreen; buffers dropped.");
		return g_fullscreenHook.call<HRESULT>(swap, full, out);
	}

	HRESULT WINAPI TargetHook(IDXGISwapChain* swap,
		const DXGI_MODE_DESC* desc)
	{
		AcquireSRWLockExclusive(&g_lock);
		DropBackbuffers();
		ReleaseSRWLockExclusive(&g_lock);
		LogInfo("ResizeTarget; buffers dropped.");
		return g_targetHook.call<HRESULT>(swap, desc);
	}

	LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		return DefWindowProcW(h, m, w, l);
	}

	bool ResolveAndHook()
	{
		// present addr off a dummy d3d11 swapchain.
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DummyWndProc;
		wc.hInstance = OwnModule();
		wc.lpszClassName = L"HJDummy";
		RegisterClassExW(&wc);
		HWND dummyWnd = CreateWindowExW(0, L"HJDummy", L"", WS_OVERLAPPEDWINDOW,
			0, 0, 16, 16, nullptr, nullptr, OwnModule(), nullptr);
		if (!dummyWnd)
		{
			LogInfo("dummy window failed.");
			return false;
		}
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferCount = 1;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = dummyWnd;
		sd.SampleDesc.Count = 1;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		ID3D11Device* d11dev = nullptr;
		IDXGISwapChain* d11swap = nullptr;
		D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,
			D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
			D3D11_SDK_VERSION, &sd, &d11swap, &d11dev, nullptr, nullptr);
		void* presentAddr = nullptr;
		void* fullscreenAddr = nullptr;
		void* resizeAddr = nullptr;
		void* targetAddr = nullptr;
		if (SUCCEEDED(hr) && d11swap)
		{
			void** vt = *reinterpret_cast<void***>(d11swap);
			presentAddr = vt[8];
			fullscreenAddr = vt[10];
			resizeAddr = vt[13];
			targetAddr = vt[14];
		}
		if (d11swap) d11swap->Release();
		if (d11dev) d11dev->Release();
		DestroyWindow(dummyWnd);
		UnregisterClassW(L"HJDummy", OwnModule());
		if (!presentAddr)
		{
			LogInfo("Present address not found.");
			return false;
		}

		// same trick for ExecuteCommandLists off a dummy d3d12 queue.
		ID3D12Device* d12dev = nullptr;
		hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&d12dev));
		void* execAddr = nullptr;
		if (SUCCEEDED(hr) && d12dev)
		{
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			ID3D12CommandQueue* q = nullptr;
			if (SUCCEEDED(d12dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q)
			{
				void** vt = *reinterpret_cast<void***>(q);
				execAddr = vt[10];
				q->Release();
			}
			d12dev->Release();
		}
		if (!execAddr)
		{
			LogInfo("ExecuteCommandLists address not found.");
			return false;
		}

		g_presentHook = safetyhook::create_inline(
			presentAddr, reinterpret_cast<void*>(&PresentHook));
		if (!g_presentHook)
		{
			LogInfo("Present hook failed.");
			return false;
		}
		g_executeHook = safetyhook::create_inline(
			execAddr, reinterpret_cast<void*>(&ExecuteHook));
		if (!g_executeHook)
		{
			LogInfo("Execute hook failed.");
			return false;
		}
		// transition guards: best effort, overlay works without em but
		// flips can freeze with our refs held. dxgi vtables dont move
		// per game patch so these are cheap to keep.
		if (fullscreenAddr)
		{
			g_fullscreenHook = safetyhook::create_inline(
				fullscreenAddr, reinterpret_cast<void*>(&FullscreenHook));
			if (!g_fullscreenHook)
				LogInfo("fullscreen hook failed; flips may freeze.");
		}
		if (resizeAddr)
		{
			g_resizeHook = safetyhook::create_inline(
				resizeAddr, reinterpret_cast<void*>(&ResizeHook));
			if (!g_resizeHook)
				LogInfo("resize hook failed; resizes may freeze.");
		}
		if (targetAddr)
		{
			g_targetHook = safetyhook::create_inline(
				targetAddr, reinterpret_cast<void*>(&TargetHook));
			if (!g_targetHook)
				LogInfo("target hook failed.");
		}

		std::ostringstream m;
		m << "hooked Present + ExecuteCommandLists + resize guards.";
		LogInfo(m.str());
		return true;
	}
}

namespace HJ::HUD::FrameOverlay
{
	bool Initialize()
	{
		if (!ResolveAndHook())
		{
			Logger::Warning("[FRAME OVERLAY] init failed; game untouched.");
			return false;
		}
		Logger::Info("[FRAME OVERLAY] ready (F11 reloads DiscOverlay.ini).");
		return true;
	}
}
