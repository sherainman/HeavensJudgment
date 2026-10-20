#include "HactOverlay.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>
#include <safetyhook.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <thread>

#include <intrin.h>

namespace
{
	using namespace HJ::HUD::HactOverlay;

	std::uintptr_t g_moduleBase = 0;

	// these are all pointer slots, the blocks live on the heap.
	constexpr std::uintptr_t kFlagsSlotRva  = 0x0430BE48;
	constexpr std::uintptr_t kLatchSlotRva  = 0x0430BEC0;
	constexpr std::uintptr_t kBattleCtxRva  = 0x041F0BD0;

	constexpr std::uintptr_t kFlagMaster   = 0x438;
	constexpr std::uintptr_t kFlagSuppress = 0x3D8;

	constexpr std::uintptr_t kLatchArmed = 0x2C;
	constexpr std::uintptr_t kLatchId    = 0x1110;

	// FighterSlot_ByIndex(1, ctx) is just ctx + 0x390
	constexpr std::uintptr_t kPlayerSlotOff = 0x390;
	constexpr std::uintptr_t kHeatCurrent   = 0x28;
	constexpr std::uintptr_t kHeatMax       = 0x2C;
	constexpr std::uintptr_t kHeatPrev      = 0x30;

	bool g_initialized = false;
	std::mutex g_logMutex;
	bool g_warnedNoBase = false;
	bool g_warnedNoCtx  = false;
	bool g_warnedNoLatch = false;

	constexpr std::uintptr_t kGateRva = 0x02EAAEB0;
	constexpr std::uintptr_t kProbeRva = 0x02F00A90;

	SafetyHookInline g_probeHook{};
	std::atomic<std::uint64_t> g_probeCalls{ 0 };

	bool IsReadable(std::uintptr_t p);

	void PromptProbeHook(void* self)
	{
		const auto n = g_probeCalls.fetch_add(1, std::memory_order_relaxed) + 1;

		const auto p = reinterpret_cast<std::uintptr_t>(self);
		const auto readable = self && IsReadable(p + 0x2A);

		std::uint32_t id = 0;
		std::uint32_t prop = 0;
		int before = -1;
		int after = -1;

		if (readable)
		{
			before = *reinterpret_cast<const int*>(p + 0x2A);

			if (IsReadable(p + 0x114))
			{
				id = *reinterpret_cast<const std::uint32_t*>(p + 0x110);
				prop = *reinterpret_cast<const std::uint32_t*>(p + 0x114);
			}
		}

		g_probeHook.call<void*>(self);

		if (readable)
			after = *reinterpret_cast<const int*>(p + 0x2A);

		if (n <= 12)
		{
			std::ostringstream line;
			line
				<< "[HACT PROBE] n=" << n
				<< " self=0x" << std::hex << p << std::dec
				<< " id=" << id
				<< " prop=" << prop
				<< " flag2A " << before << "->" << after;

			std::lock_guard<std::mutex> lock(g_logMutex);
			HJ::Logger::Info(line.str());
		}
	}

	SafetyHookInline g_gateHook{};

	std::atomic<std::uint32_t> g_pendingId{ 0 };
	std::atomic<std::uint32_t> g_pendingProp{ 0 };
	std::atomic<bool>     g_havePending{ false };

	std::atomic<bool>     g_observed{ false };
	std::atomic<bool>     g_lastAvailable{ false };
	std::atomic<std::uint32_t> g_lastId{ 0 };
	std::atomic<std::uint32_t> g_lastProp{ 0 };
	std::atomic<ULONGLONG> g_lastAt{ 0 };

	constexpr ULONGLONG kVerdictBackstopMs = 30000;
	std::atomic<bool>     g_clearLogged{ false };

	using S = HactRoute;

	constexpr HactRoute kRoutes[] =
	{
		//  id,  prop, required states
		{ 4098,  0, S::kDown | S::kDragon | S::kRun | S::kHeatOrDragon },
		{ 4101,  0, S::kDown | S::kDragon | S::kRun | S::kHeatOrDragon },
		{ 4103,  0, S::kDown | S::kDragon | S::kHeatOrDragon },
		{ 6161,  0, S::kDown | S::kHeatOrDragon },
		{ 6162,  0, S::kDown | S::kHeatOrDragon },
		{ 6167,  0, S::kDown | S::kHeatOrDragon },
		{ 6172,  0, S::kDown | S::kHeatOrDragon },
		{ 6181,  0, S::kDown | S::kDragon | S::kRun | S::kHeatOrDragon },
		{ 6244,  0, S::kDown | S::kDrunkLimit | S::kHeatOrDragon },
		{ 6245,  0, S::kDown | S::kDrunkLimit | S::kHeatOrDragon },
		{ 6246,  0, S::kDown | S::kDrunkLimit | S::kHeatOrDragon },
		{ 6247,  0, S::kDown | S::kHeatMax | S::kKamae | S::kVRBattle | S::kHeatOrDragon },
		{ 6252,  0, S::kDown | S::kDragon | S::kHeatOrDragon },
		{ 6258,  0, S::kDown | S::kKamae | S::kAgony | S::kHeatOrDragon },
		{ 6260,  0, S::kDown | S::kHeatOrDragon },
		{ 6674,  0, S::kDown | S::kDownAo | S::kHeatOrDragon },
		{ 6675,  0, S::kDown | S::kDownUtu | S::kHeatOrDragon },
		{ 6676,  0, S::kDown | S::kHPPinch | S::kHeatOrDragon },
		{ 6791,  0, S::kDown | S::kHeatOrDragon },
		{ 6866,  0, S::kDown | S::kHeatOrDragon },
		{ 6952,  0, S::kDown | S::kHeatOrDragon },
		{ 6963,  0, S::kDown | S::kHeatOrDragon },
		{ 7543,  0, S::kDown | S::kHeatOrDragon },
		{ 7716,  0, S::kDown | S::kHeatOrDragon },
		{ 8322,  0, S::kDown | S::kRecoverDead | S::kHeatOrDragon },
		{ 8495,  0, S::kAgony | S::kHeatOrDragon },
		{ 8833,  0, S::kDown | S::kGuardBreak | S::kHeatOrDragon },
		{ 9882,  0, S::kDown | S::kDragon | S::kWallBound | S::kHeatOrDragon },
		{10398,  0, S::kDown | S::kDragon | S::kAgony | S::kHeatOrDragon },
		{10399,  0, S::kDown | S::kDragon | S::kHPPinch | S::kHeatOrDragon },
		{ 7544,  0, S::kDown | S::kHeatOrDragon },
		{ 7544,  2, S::kDown | S::kHeatOrDragon },
		{ 7544,  3, S::kDown | S::kHeatOrDragon },
		{ 7544,  4, S::kDown | S::kHeatOrDragon },
		{ 7544,  5, S::kDown | S::kHeatOrDragon },
		{ 7544,  6, S::kDown | S::kHeatOrDragon },
	};

	std::uintptr_t ReadPtr(std::uintptr_t rva)
	{
		if (!g_moduleBase)
			return 0;
		return *reinterpret_cast<std::uintptr_t*>(g_moduleBase + rva);
	}

	bool IsReadable(std::uintptr_t p)
	{
		if (!p)
			return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == 0)
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		const auto protect = mbi.Protect & 0xFF;
		return
			protect == PAGE_READONLY ||
			protect == PAGE_READWRITE ||
			protect == PAGE_WRITECOPY ||
			protect == PAGE_EXECUTE_READ ||
			protect == PAGE_EXECUTE_READWRITE ||
			protect == PAGE_EXECUTE_WRITECOPY;
	}

	struct GateSample
	{
		bool ok = false;
		int master = -1;    // -1 == unreadable
		int suppress = -1;
		int armed = -1;
		unsigned latchedId = 0;
	};

	void SampleGate(GateSample& out)
	{
		out.ok = false;
		out.master = -1;
		out.suppress = -1;
		out.armed = -1;
		out.latchedId = 0;

		__try
		{
			const auto flags = ReadPtr(kFlagsSlotRva);
			if (!flags)
				return;

			out.master =
				*reinterpret_cast<const int*>(flags + kFlagMaster);
			out.suppress =
				*reinterpret_cast<const int*>(flags + kFlagSuppress);

			const auto latch = ReadPtr(kLatchSlotRva);
			if (latch)
			{
				out.armed =
					*reinterpret_cast<const int*>(latch + kLatchArmed);
				out.latchedId =
					*reinterpret_cast<const unsigned*>(latch + kLatchId);
			}

			out.ok = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out.ok = false;
		}
	}

	void ClearVerdict(const char* why, unsigned latchedId)
	{
		const auto id = g_lastId.load(std::memory_order_relaxed);
		const auto prop = g_lastProp.load(std::memory_order_relaxed);

		g_observed.store(false, std::memory_order_release);
		g_lastAt.store(0, std::memory_order_release);
		g_lastAvailable.store(false, std::memory_order_release);

		if (g_clearLogged.exchange(true, std::memory_order_acq_rel))
			return;

		std::ostringstream line;
		line
			<< "[HACT OVERLAY] CLEAR via=" << why
			<< " id=" << id
			<< " prop=" << prop
			<< " latchedId=" << latchedId;

		std::lock_guard<std::mutex> lock(g_logMutex);
		HJ::Logger::Info(line.str());
	}

	std::atomic<std::uint64_t> g_calls{ 0 };
	std::atomic<std::uint64_t> g_entryPasses{ 0 };
	std::atomic<std::uint64_t> g_returnPasses{ 0 };
	std::atomic<std::uint64_t> g_positives{ 0 };
	std::atomic<std::uint64_t> g_negatives{ 0 };
	std::atomic<bool> g_lapseLogged{ false };
	std::atomic<std::uint64_t> g_unknowRoutes{ 0 };
	std::atomic<bool> g_dumpedFirst{ false };

	struct KnownCaller
	{
		std::uintptr_t start;
		std::uintptr_t end; // exclusive
		const char* name;
	};

	// measured function bodies, not windows. FUN_142f00a90 alone is 0xC39
	// bytes, so a guessed size silently reports real callers as unknown.
	constexpr KnownCaller kKnownCallers[] =
	{
		{ 0x02229380, 0x02229DFE, "FUN_142229380" },
		{ 0x02317290, 0x02317679, "FUN_142317290" },
		{ 0x02A70010, 0x02A7066E, "FUN_142A70010" },
		{ 0x02EBD380, 0x02EBDE2F, "FUN_142EBD380" },
		{ 0x02F00A90, 0x02F016C9, "FUN_142F00A90" },
	};

	const char* NameCaller(std::uintptr_t rva)
	{
		for (const auto& k : kKnownCallers)
		{
			if (rva >= k.start && rva < k.end)
				return k.name;
		}
		return "UNRECOGNISED";
	}

	void LogFirstCalls(std::uint64_t raw, std::uint32_t id, std::uint32_t prop,
			std::uintptr_t callerRva)
	{
		if (g_dumpedFirst.load(std::memory_order_relaxed))
			return;
		if (g_calls.load(std::memory_order_relaxed) > 8)
			return;

		GateSample sample{};
		SampleGate(sample);

		std::ostringstream line;
		line
			<< "[HACT OVERLAY] gate n="
			<< g_calls.load(std::memory_order_relaxed)
			<< " raw=0x" << std::hex << raw << std::dec
			<< " recId=" << id
			<< " recProp=" << prop
			<< " | master=" << sample.master
			<< " suppress=" << sample.suppress
			<< " armed=" << sample.armed
			<< " latchId=" << sample.latchedId
			<< (sample.latchedId > 99999 ? "(OUT OF ID RANGE)" : "")
			<< " caller=" << callerRva
			<< " " << NameCaller(callerRva)
			<< ((callerRva >= 0x1000 && callerRva <= 0x33E2200)
				? "" : " [TRAMPOLINE?]");

		std::lock_guard<std::mutex> lock(g_logMutex);
		HJ::Logger::Info(line.str());

		if (g_calls.load(std::memory_order_relaxed) >= 8)
			g_dumpedFirst.store(true, std::memory_order_relaxed);
	}

	std::uint64_t __fastcall GateHook(
		void* latch, void* record)
	{
		thread_local bool inHook = false;
		if (inHook)
			return g_gateHook.call<std::uint64_t>(latch, record);

		const auto callerRva =
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()) -
			(g_moduleBase ? g_moduleBase : 0);

		inHook = true;

		const auto n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
		g_entryPasses.fetch_add(1, std::memory_order_relaxed);

		std::uint32_t id = 0;
		std::uint32_t prop = 0;

		if (record && IsReadable(
			reinterpret_cast<std::uintptr_t>(record) + 0xE4))
		{
			const auto rec = reinterpret_cast<std::uintptr_t>(record);
			id = *reinterpret_cast<const std::uint32_t*>(rec + 0xE0);
			prop = *reinterpret_cast<const std::uint32_t*>(rec + 0xE4);
		}

		const auto result = g_gateHook.call<std::uint64_t>(latch, record);

		inHook = false;

		g_returnPasses.fetch_add(1, std::memory_order_relaxed);

		if (n <= 8)
			LogFirstCalls(result, id, prop, callerRva);

		if ((result & 0xFF) == 0)
		{
			g_negatives.fetch_add(1, std::memory_order_relaxed);

			if (g_observed.load(std::memory_order_acquire) &&
				!g_lapseLogged.exchange(true, std::memory_order_acq_rel))
			{
				std::ostringstream line;
				line
					<< "[HACT OVERLAY] LAPSE: negative verdict after "
					<< "positive, id="
					<< g_lastId.load(std::memory_order_relaxed)
					<< " prop="
					<< g_lastProp.load(std::memory_order_relaxed);

				std::lock_guard<std::mutex> lock(g_logMutex);
				HJ::Logger::Info(line.str());
			}

			return result;
		}

		g_positives.fetch_add(1, std::memory_order_relaxed);

		const auto* route = HJ::HUD::HactOverlay::FindRoute(id, prop);
		if (!route)
			g_unknowRoutes.fetch_add(1, std::memory_order_relaxed);

		GateSample sample{};
		SampleGate(sample);

		{
			std::ostringstream line;
			line
				<< "[HACT OVERLAY] AVAILABLE id=" << id
				<< " prop=" << prop
				<< " raw=0x" << std::hex << result << std::dec
				<< (route ? " [tracked]" : " [NOT in our 36]")
				<< " | master=" << sample.master
				<< " suppress=" << sample.suppress
				<< " armed=" << sample.armed
				<< " latchId=" << sample.latchedId;
			std::lock_guard<std::mutex> lock(g_logMutex);
			HJ::Logger::Info(line.str());
		}

		g_lastId.store(id, std::memory_order_relaxed);
		g_lastProp.store(prop, std::memory_order_relaxed);
		g_lastAvailable.store(true, std::memory_order_release);
		g_lastAt.store(GetTickCount64(), std::memory_order_release);
		g_observed.store(true, std::memory_order_release);

		g_clearLogged.store(false, std::memory_order_release);

		return result;
	}

	void WarnOnce(bool& flag, const char* msg)
	{
		if (flag)
			return;
		flag = true;
		std::lock_guard<std::mutex> lock(g_logMutex);
		HJ::Logger::Warning(
			std::string("[HACT OVERLAY] ") + msg
		);
	}
}

namespace HJ::HUD::HactOverlay
{
	const HactRoute* Routes()
	{
		return kRoutes;
	}

	std::size_t RouteCount()
	{
		return std::size(kRoutes);
	}

	const HactRoute* FindRoute(std::uint32_t id, std::uint32_t prop)
	{
		for (const auto& r : kRoutes)
		{
			if (r.id == id && r.prop == prop)
				return &r;
		}
		return nullptr;
	}

	State ComputeState()
	{
		State s{};

		// heat is only for the gauge and the rose, safe to read with nothing
		// latched.
		if (g_moduleBase)
		{
			const auto ctx = ReadPtr(kBattleCtxRva);
			if (ctx)
			{
				const auto fighter = ctx + kPlayerSlotOff;
				if (IsReadable(fighter + kHeatMax))
				{
					const float cur =
						*reinterpret_cast<const float*>(fighter + kHeatCurrent);
					const float max =
						*reinterpret_cast<const float*>(fighter + kHeatMax);
					const float prev =
						*reinterpret_cast<const float*>(fighter + kHeatPrev);

					s.heatRatio = (max <= 0.0f) ? 0.0f : (cur / max);
					s.tier = static_cast<int>(cur);
					s.rose = (prev <= cur);
				}
			}
			else if (!g_warnedNoCtx)
			{
				WarnOnce(g_warnedNoCtx, "battle context null, heat fields unavailable.");
			}
		}
		else if (!g_warnedNoBase)
		{
			WarnOnce(g_warnedNoBase, "module base not set.");
		}

		// same order as 0x142EAB760.
		const auto flags = ReadPtr(kFlagsSlotRva);
		if (!IsReadable(flags + kFlagMaster))
			return s;

		s.masterEnable = (*reinterpret_cast<const std::uint8_t*>(
			flags + kFlagMaster) != 0);
		s.suppressed = (*reinterpret_cast<const std::uint8_t*>(
			flags + kFlagSuppress) != 0);

		if (!s.masterEnable || s.suppressed)
			return s;

		const auto latch = ReadPtr(kLatchSlotRva);
		if (!IsReadable(latch + kLatchId))
		{
			if (!g_warnedNoLatch)
				WarnOnce(g_warnedNoLatch, "latch block unreadable.");
			return s;
		}

		s.latchedId = *reinterpret_cast<const std::uint32_t*>(
			latch + kLatchId);

		// the latch is a single slot, so a non-zero id means ours got claimed.
		// assumed, not verified: that it stays 0 while merely available. if
		// its backwards the overlay never appears and the CLEAR line says so.
		if (s.latchedId != 0 &&
			g_observed.load(std::memory_order_acquire))
		{
			ClearVerdict("latch-claimed", s.latchedId);
			return s;
		}

		if (!g_observed.load(std::memory_order_acquire))
			return s;

		// the gate already ran every per-HAct condition, so report its answer
		// instead of reimplementing it. it never re-polls, so this is sticky.
		const auto at = g_lastAt.load(std::memory_order_acquire);
		const auto now = GetTickCount64();

		if (now < at)
			return s;

		if ((now - at) > kVerdictBackstopMs)
		{
			ClearVerdict("backstop-timeout", s.latchedId);
			return s;
		}

		s.routeId = g_lastId.load(std::memory_order_relaxed);
		s.routeProp = g_lastProp.load(std::memory_order_relaxed);
		s.available = true;
		s.resolved = true;

		// the gate also fires for routes that are not ours, dont draw those.
		if (FindRoute(s.routeId, s.routeProp) == nullptr)
		{
			s.available = false;
			s.resolved = false;
			s.routeId = 0;
			s.routeProp = 0;
		}

		return s;
	}

	State GetState()
	{
		const State s = ComputeState();

		// heartbeat separates "hook never fired" from "fired, nothing was
		// available", and ageMs shows how long a verdict has been held.
		static ULONGLONG lastBeat = 0;
		const auto nowBeat = GetTickCount64();

		if (lastBeat == 0)
		{
			lastBeat = nowBeat;
		}
		else if ((nowBeat - lastBeat) > 10000 &&
			g_calls.load(std::memory_order_relaxed) > 0)
		{
			lastBeat = nowBeat;

			const auto at = g_lastAt.load(std::memory_order_acquire);
			const auto age =
				(at != 0 && nowBeat >= at) ? (nowBeat - at) : 0;

			std::ostringstream line;
			line
				<< "[HACT OVERLAY] hb calls="
				<< g_calls.load(std::memory_order_relaxed)
				<< " positives=" << g_positives.load(std::memory_order_relaxed)
				<< " neg=" << g_negatives.load(std::memory_order_relaxed)
				<< " probe=" << g_probeCalls.load(std::memory_order_relaxed)
				<< " untracked=" << g_unknowRoutes.load(std::memory_order_relaxed)
				<< " master=" << (s.masterEnable ? 1 : 0)
				<< " latchId=" << s.latchedId
				<< " held=" << (s.available ? 1 : 0)
				<< " route=" << s.routeId
				<< " ageMs=" << age;

			std::lock_guard<std::mutex> lock(g_logMutex);
			HJ::Logger::Info(line.str());
		}

		return s;
	}

	bool Initialize()
	{
		if (g_initialized)
			return true;

		g_moduleBase = reinterpret_cast<std::uintptr_t>(
			GetModuleHandleA(nullptr));

		if (!g_moduleBase)
		{
			HJ::Logger::Error("[HACT OVERLAY] no main module.");
			return false;
		}

		std::lock_guard<std::mutex> lock(g_logMutex);

		const auto gateTarget = g_moduleBase + kGateRva;

		g_gateHook = safetyhook::create_inline(
			reinterpret_cast<void*>(gateTarget),
			reinterpret_cast<void*>(&GateHook)
		);

		if (!g_gateHook)
		{
			HJ::Logger::Error(
				"[HACT OVERLAY] gate hook failed - "
				"overlay will stay hidden."
			);
			return false;
		}

		// call frequency is the only way to tell the availability path from
		// the execution path, since there is no vanilla popup to watch.
		const auto probeTarget = g_moduleBase + kProbeRva;

		g_probeHook = safetyhook::create_inline(
			reinterpret_cast<void*>(probeTarget),
			reinterpret_cast<void*>(&PromptProbeHook)
		);

		if (!g_probeHook)
		{
			HJ::Logger::Error(
				"[HACT OVERLAY] frequency probe failed - "
				"diagnostics degraded, overlay unaffected."
			);
		}

		HJ::Logger::Info(
			"[HACT OVERLAY] ready. 36 routes, boxing-only. "
			"Resolver hooked on FUN_142EAAEB0 (image+0x02EAAEB0); "
			"frequency probe on FUN_142F00A90 (image+0x02F00A90). "
			"Verdict STICKY, 30s backstop. AUDIBLE CUE (scaffolding)."
		);

		g_initialized = true;
		return true;
	}

	void Shutdown()
	{
		if (g_gateHook)
		{
			g_gateHook = {};
		}

		g_observed.store(false, std::memory_order_release);
		g_lastAvailable.store(false, std::memory_order_release);
		g_havePending.store(false, std::memory_order_relaxed);
		g_lastAt.store(0, std::memory_order_release);

		g_initialized = false;
		g_moduleBase = 0;
	}
}
