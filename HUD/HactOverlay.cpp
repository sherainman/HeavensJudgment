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

	bool TryReadByte(std::uintptr_t address, std::uint8_t& value);
	bool TryReadUInt32(std::uintptr_t address, std::uint32_t& value);
	bool TryReadFloat(std::uintptr_t address, float& value);

	// the gate runs once per live HAct record, so it is the hottest code in
	// the overlay. SEH instead of VirtualQuery: the old readability check
	// was a kernel transition on every poll and cost more than the rest of
	// the hook combined. leaf function keeps SEH out of the callers that
	// build C++ objects.
	bool ReadRecordIdentity(
		void* record,
		std::uint32_t& id,
		std::uint32_t& prop)
	{
		if (!record)
			return false;

		__try
		{
			const auto rec =
				reinterpret_cast<std::uintptr_t>(record);

			id = *reinterpret_cast<const std::uint32_t*>(rec + 0xE0);
			prop = *reinterpret_cast<const std::uint32_t*>(rec + 0xE4);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			id = 0;
			prop = 0;
			return false;
		}

		return true;
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

	// per-frame reads use SEH, not VirtualQuery. VirtualQuery is a kernel
	// transition and the game's protection makes it cost milliseconds, so
	// polling it three times a frame was the entire frame budget. these
	// leaves keep SEH out of ComputeState, which builds no objects but is
	// still easier to read without it.
	bool TryReadByte(std::uintptr_t address, std::uint8_t& value)
	{
		__try
		{
			value = *reinterpret_cast<const std::uint8_t*>(
				address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
			return false;
		}
	}

	bool TryReadUInt32(std::uintptr_t address, std::uint32_t& value)
	{
		__try
		{
			value = *reinterpret_cast<const std::uint32_t*>(
				address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
			return false;
		}
	}

	bool TryReadFloat(std::uintptr_t address, float& value)
	{
		__try
		{
			value = *reinterpret_cast<const float*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0.0f;
			return false;
		}
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

		inHook = true;

		const auto n =
			g_calls.fetch_add(1, std::memory_order_relaxed) + 1;

		const auto result =
			g_gateHook.call<std::uint64_t>(latch, record);

		inHook = false;

		// hot path: nearly every poll is a negative, and the overlay only
		// acts on positives. keep this branch to an atomic and a compare --
		// no memory reads, no formatting, no kernel calls.
		if ((result & 0xFF) == 0)
		{
			g_negatives.fetch_add(1, std::memory_order_relaxed);

			if (n <= 8)
			{
				std::uint32_t id = 0;
				std::uint32_t prop = 0;
				ReadRecordIdentity(record, id, prop);

				LogFirstCalls(
					result,
					id,
					prop,
					reinterpret_cast<std::uintptr_t>(_ReturnAddress()) -
						(g_moduleBase ? g_moduleBase : 0)
				);
			}

			if (g_observed.load(std::memory_order_acquire) &&
				!g_lapseLogged.load(std::memory_order_relaxed) &&
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

		std::uint32_t id = 0;
		std::uint32_t prop = 0;
		ReadRecordIdentity(record, id, prop);

		const auto* route = HJ::HUD::HactOverlay::FindRoute(id, prop);
		if (!route)
			g_unknowRoutes.fetch_add(1, std::memory_order_relaxed);

		// a prompt can stay available for many frames, so one line per
		// change (or per 5s) is enough to debug with and keeps the string
		// building off the repeat path.
		const auto now = GetTickCount64();
		static std::atomic<ULONGLONG> lastLog{ 0 };
		static std::atomic<std::uint64_t> lastKey{ ~0ull };

		const auto key =
			(static_cast<std::uint64_t>(id) << 32) | prop;

		if (key != lastKey.load(std::memory_order_relaxed) ||
			now - lastLog.load(std::memory_order_relaxed) > 5000)
		{
			lastKey.store(key, std::memory_order_relaxed);
			lastLog.store(now, std::memory_order_relaxed);

			GateSample sample{};
			SampleGate(sample);

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
		g_lastAt.store(now, std::memory_order_release);
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

				float cur = 0.0f;
				float max = 0.0f;
				float prev = 0.0f;

				if (TryReadFloat(fighter + kHeatCurrent, cur) &&
					TryReadFloat(fighter + kHeatMax, max) &&
					TryReadFloat(fighter + kHeatPrev, prev))
				{
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

		std::uint8_t master = 0;
		std::uint8_t suppress = 0;

		if (!flags ||
			!TryReadByte(flags + kFlagMaster, master) ||
			!TryReadByte(flags + kFlagSuppress, suppress))
			return s;

		s.masterEnable = master != 0;
		s.suppressed = suppress != 0;

		if (!s.masterEnable || s.suppressed)
			return s;

		const auto latch = ReadPtr(kLatchSlotRva);

		std::uint32_t latchedId = 0;

		if (!latch ||
			!TryReadUInt32(latch + kLatchId, latchedId))
		{
			if (!g_warnedNoLatch)
				WarnOnce(g_warnedNoLatch, "latch block unreadable.");
			return s;
		}

		s.latchedId = latchedId;

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

		HJ::Logger::Info(
			"[HACT OVERLAY] ready. 36 routes, boxing-only. "
			"Resolver hooked on FUN_142EAAEB0 (image+0x02EAAEB0). "
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
