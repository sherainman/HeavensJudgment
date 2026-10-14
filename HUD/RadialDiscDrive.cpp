#include "RadialDiscDrive.hpp"

#include "RadialHealthBridge.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace
{
	std::uintptr_t g_moduleBase = 0;

	// rechecked every call so a recycled slot cant get written by mistake.
	std::uintptr_t g_cachedObject = 0;

	std::atomic<bool> g_loggedFound{ false };


	// per-frame caller, so cap the spam. failures always log.
	std::atomic<std::uint32_t> g_driveCount{ 0 };

	std::atomic<std::uint32_t> g_logCount{ 0 };

	std::atomic<float> g_lastLoggedRatio{ -1.0f };

	// same uimanager constant as radialdisc / boxing trace.
	constexpr std::uintptr_t kUiManagerRva = 0x041F0D80;

	// RCX = &handle, XMM1 = value (0x1427CABE0, slot vtable+0x2C0).
	constexpr std::uintptr_t kSetValueRva = 0x027CABE0;

	// class vtables by c87: 0 default, 1 radial, 2 bar
	constexpr std::uintptr_t kDefaultClassVtable = 0x1435EE198;
	// RADIAL class vtable (c87=1)
	constexpr std::uintptr_t kRadialClassVtable = 0x1435F3BC8;
	// BAR class vtable (c87=2) - health bar
	constexpr std::uintptr_t kBarClassVtable = 0x1435ED368;

	// our vinyl_radial layer runs DEFAULT class
	constexpr std::uintptr_t kExpectedVtable = kDefaultClassVtable;

	// slot is just MOV RAX,[RCX] / JMP [RAX+0x2C0]. call it by hand
	// with RCX = object, XMM1 = value.
	constexpr std::uintptr_t kSetValueSlotOffset = 0x2C0;

	// object table + per-object fields, from the live dump.
	constexpr std::uintptr_t kMgrObjects = 0x190;
	constexpr std::uintptr_t kMgrCapacity = 0x19C;
	constexpr std::uintptr_t kObjHandleOffset = 0x70;
	constexpr std::uintptr_t kObjTagOffset = 0xB8;

	constexpr std::uintptr_t kObjVtableOffset = 0x0;

	// type-1 handler derefs *(obj+0x340) with no null check. verify it
	// first or it faults.
	constexpr std::uintptr_t kObjRadialChildOffset = 0x340;

	std::atomic<bool> g_loggedNoChild{ false };
	std::atomic<bool> g_disabled{ false };

	// scene 301, layer 28 (vinyl_radial).
	constexpr std::uint32_t kSceneLifeGauge = 0x12D; // 301
	constexpr std::uint32_t kRadialLayer = 28;

	constexpr std::uint64_t kTargetTag =
		(static_cast<std::uint64_t>(kRadialLayer) << 32) |
		kSceneLifeGauge;

	// +0x1C flag: 0x0 fighting, 0x1 menu-over-hud, 0x100 hidden.
	constexpr std::uint64_t kRootTag =
		(static_cast<std::uint64_t>(1) << 32) |
		kSceneLifeGauge;
	constexpr std::uintptr_t kObjShownFlagOffset = 0x1C;
	constexpr std::uint32_t kHiddenBit = 0x100;

	// F7/F8 snap 0.0/1.0, F9 next target, F10 back to layer 28.
	// debug scaffolding, runs on the next damage event.
	std::atomic<float> g_testSnap{ std::nanf("") };
	// retarget list: the gauge + the 5 mask pieces. -1 = default.
	std::atomic<int> g_testTarget{ -1 };
	struct TestCandidate { std::uint32_t tagHigh; std::uintptr_t vtable; const char* label; };
	const TestCandidate kTestCandidates[] = {
		{ 0x1C, 0x1435EE198, "gauge/0x1C" },
		{ 0x1D, 0x1435ED368, "mask-piece#0" },
		{ 0x1D, 0x1435ED368, "mask-piece#1" },
		{ 0x1D, 0x1435ED368, "mask-piece#2" },
		{ 0x1D, 0x1435ED368, "mask-piece#3" },
		{ 0x1D, 0x1435ED368, "mask-piece#4" },
	};
	constexpr int kTestCandidateCount =
		sizeof(kTestCandidates) / sizeof(kTestCandidates[0]);

	// Nth match (0-based) for the active candidate's tag/vtable.
	// Default target: first match, preserving old behavior exactly.
	int ActiveOrdinal()
	{
		const int t =
			g_testTarget.load(std::memory_order_relaxed);
		if (t >= 1 && t < kTestCandidateCount)
			return t - 1;
		return 0;
	}

	std::uint64_t ActiveTag()
	{
		const int t =
			g_testTarget.load(std::memory_order_relaxed);
		if (t >= 0 && t < kTestCandidateCount)
			return (static_cast<std::uint64_t>(
				kTestCandidates[t].tagHigh) << 32) | kSceneLifeGauge;
		return kTargetTag;
	}

	std::uintptr_t ActiveVtable()
	{
		const int t =
			g_testTarget.load(std::memory_order_relaxed);
		if (t >= 0 && t < kTestCandidateCount)
			return kTestCandidates[t].vtable;
		return kExpectedVtable;
	}

	void PollTestKeys()
	{
		static bool prevF7 = false, prevF8 = false;
		static bool prevF9 = false, prevF10 = false;
		const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
		const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
		const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
		const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
		if (f7 && !prevF7)
		{
			g_testSnap.store(0.0f, std::memory_order_relaxed);
			std::ostringstream s;
			s << "[RADIAL TEST] snap=0.0 (F7)";
			HJ::Logger::Info(s.str());
		}
		if (f8 && !prevF8)
		{
			g_testSnap.store(1.0f, std::memory_order_relaxed);
			std::ostringstream s;
			s << "[RADIAL TEST] snap=1.0 (F8)";
			HJ::Logger::Info(s.str());
		}
		if (f9 && !prevF9)
		{
			int t = g_testTarget.load(std::memory_order_relaxed);
			t = (t + 1) % kTestCandidateCount;
			g_testTarget.store(t, std::memory_order_relaxed);
			g_cachedObject = 0;
			std::ostringstream s;
			s << "[RADIAL TEST] target=" << kTestCandidates[t].label
			  << " (F9)";
			HJ::Logger::Info(s.str());
		}
		if (f10 && !prevF10)
		{
			g_testTarget.store(-1, std::memory_order_relaxed);
			g_testSnap.store(std::nanf(""), std::memory_order_relaxed);
			g_cachedObject = 0;
			HJ::Logger::Info("[RADIAL TEST] reset to default (F10)");
		}
		prevF7 = f7; prevF8 = f8; prevF9 = f9; prevF10 = f10;
	}

	// x64 passes FP args by position: (handle, float) = RCX, XMM1.
	// value MUST be the first float or set_value writes 0.0 forever
	// (looks exactly like "wont write at all").
	using SetValueFn = void(__fastcall*)(
		const std::uint64_t* handlePtr,
		float value);

	
	static std::uintptr_t SafeReadPtr(const std::uintptr_t addr)
	{
		__try
		{
			return *reinterpret_cast<const std::uintptr_t*>(addr);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	static std::uint32_t SafeRead32(const std::uintptr_t addr)
	{
		__try
		{
			return *reinterpret_cast<const std::uint32_t*>(addr);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	static std::uint64_t SafeRead64(const std::uintptr_t addr)
	{
		__try
		{
			return *reinterpret_cast<const std::uint64_t*>(addr);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

std::uintptr_t FindRadialObject(std::uint64_t* outHandle)
{
	*outHandle = 0;

	const auto uiManager = SafeReadPtr(g_moduleBase + kUiManagerRva);
	if (!uiManager)
		return 0;

	const auto objects = SafeReadPtr(uiManager + kMgrObjects);
	const auto capacity = SafeRead32(uiManager + kMgrCapacity);

	if (!objects || capacity == 0 || capacity > 0x1000000)
		return 0;

	for (std::uint32_t index = 0, seen = 0; index < capacity; ++index)
	{
		const auto object = SafeReadPtr(objects + static_cast<std::uintptr_t>(index) * 8);
		if (!object)
			continue;

		if (SafeRead64(object + kObjTagOffset) != ActiveTag())
			continue;

	// same tag can match several objects, so check vtable + ordinal too.
		if (SafeReadPtr(object + kObjVtableOffset) != ActiveVtable())
			continue;

		const auto handle = SafeRead64(object + kObjHandleOffset);

		// engine self-check: low word of handle must be index+1.
		if (!handle ||
			static_cast<std::uint32_t>(handle) != index + 1)
			continue;

		if (seen++ < static_cast<std::uint32_t>(ActiveOrdinal()))
			continue;

		*outHandle = handle;
		return object;
	}

	return 0;
}

	// pod only or the compiler complains (C2712). -1 on fault.
	int ProbeRootFlag(std::uintptr_t& cached, std::uint64_t& cachedHandle)
	{
		if (cached)
		{
			__try
			{
				if (*reinterpret_cast<std::uint64_t*>(
						cached + kObjTagOffset) == kRootTag &&
					*reinterpret_cast<std::uint64_t*>(
						cached + kObjHandleOffset) == cachedHandle)
				{
					return static_cast<int>(
						*reinterpret_cast<std::uint32_t*>(
							cached + kObjShownFlagOffset));
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
			cached = 0;
			cachedHandle = 0;
		}
		const auto uiManager = SafeReadPtr(g_moduleBase + kUiManagerRva);
		if (!uiManager)
			return -1;
		const auto objects = SafeReadPtr(uiManager + kMgrObjects);
		const auto capacity = SafeRead32(uiManager + kMgrCapacity);
		if (!objects || capacity == 0 || capacity > 0x1000000)
			return -1;
		for (std::uint32_t index = 0; index < capacity; ++index)
		{
			const auto object = SafeReadPtr(
				objects + static_cast<std::uintptr_t>(index) * 8);
			if (!object)
				continue;
			if (SafeRead64(object + kObjTagOffset) != kRootTag)
				continue;
			const auto handle = SafeRead64(object + kObjHandleOffset);
			if (!handle ||
				static_cast<std::uint32_t>(handle) != index + 1)
				continue;
			cached = object;
			cachedHandle = handle;
			__try
			{
				return static_cast<int>(
					*reinterpret_cast<std::uint32_t*>(
						object + kObjShownFlagOffset));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				cached = 0;
				cachedHandle = 0;
				return -1;
			}
		}
		return -1;
	}

	// SEH cant live next to C++ objects (C2712), hence the leaf helpers.
	bool ValidateCachedObject(
		std::uintptr_t& object,
		std::uint64_t& handle)
	{
		object = 0;
		handle = 0;

		__try
		{
			if (g_cachedObject)
			{
				const auto tag =
					*reinterpret_cast<std::uint64_t*>(
						g_cachedObject + kObjTagOffset);

				handle =
					*reinterpret_cast<std::uint64_t*>(
						g_cachedObject + kObjHandleOffset);

				const auto vtable =
					*reinterpret_cast<std::uintptr_t*>(
						g_cachedObject + kObjVtableOffset);

				if (tag == ActiveTag() && handle && vtable == ActiveVtable())
				{
					object = g_cachedObject;
					return true;
				}

				g_cachedObject = 0;
				handle = 0;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			g_cachedObject = 0;
			handle = 0;
		}

		return false;
	}

	// a bad radial node must never take the game down.
	bool TrySetValue(
		std::uintptr_t setValueAddress,
		std::uint64_t* handlePtr,
		float ratio)
	{
		__try
		{
			const auto setValue =
				reinterpret_cast<SetValueFn>(setValueAddress);

			setValue(handlePtr, ratio);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// sub layout: +0x08 cur, +0x28 min, +0x2C max.
	constexpr std::uintptr_t kSubCurrentValue = 0x08;
	constexpr std::uintptr_t kSubMin = 0x28;
	constexpr std::uintptr_t kSubMax = 0x2C;

	// The health controller feeds the wide bar from controller+0x180.
	constexpr std::uintptr_t kWideBarHandleOffset = 0x180;

	struct SubRange
	{
		std::uintptr_t object = 0;
		std::uintptr_t vtable = 0;
		std::uintptr_t sub = 0;
		float current = 0.0f;
		float minValue = 0.0f;
		float maxValue = 0.0f;
		bool valid = false;
	};

	// C2712 again: guarded reads live here, callers just format.
	// note: the +0x2C0 handler silently drops the value when max <= min
	// (no fault, disc just never drains). repairRange rewrites those to 0..1.
	bool ReadSubRange(
		std::uintptr_t object,
		SubRange& out,
		bool repairRange)
	{
		out.object = object;
		out.vtable = 0;
		out.sub = 0;
		out.current = 0.0f;
		out.minValue = 0.0f;
		out.maxValue = 0.0f;
		out.valid = false;

		__try
		{
			out.vtable =
				*reinterpret_cast<std::uintptr_t*>(
					object + kObjVtableOffset);

			out.sub =
				*reinterpret_cast<std::uintptr_t*>(
					object + kObjRadialChildOffset);

			if (!out.sub)
				return false;

			out.current =
				*reinterpret_cast<float*>(
					out.sub + kSubCurrentValue);

			out.minValue =
				*reinterpret_cast<float*>(
					out.sub + kSubMin);

			out.maxValue =
				*reinterpret_cast<float*>(
					out.sub + kSubMax);

			out.valid = true;

			if (repairRange && !(out.maxValue > out.minValue))
			{
				*reinterpret_cast<float*>(
					out.sub + kSubMin) = 0.0f;

				*reinterpret_cast<float*>(
					out.sub + kSubMax) = 1.0f;

				out.minValue = 0.0f;
				out.maxValue = 1.0f;
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out.valid = false;
			return false;
		}
	}

	// Handle -> object, mirroring the engine's own resolution inside
	// set_value: low32(handle)-1 indexes the uiManager object table.
	std::uintptr_t ResolveHandle(std::uint64_t handle)
	{
		__try
		{
			if (!handle)
				return 0;

			const auto uiManager =
				*reinterpret_cast<std::uintptr_t*>(
					g_moduleBase + kUiManagerRva);

			if (!uiManager)
				return 0;

			const auto objects =
				*reinterpret_cast<std::uintptr_t*>(
					uiManager + kMgrObjects);

			const auto capacity =
				*reinterpret_cast<std::uint32_t*>(
					uiManager + kMgrCapacity);

			const auto index =
				static_cast<std::uint32_t>(handle) - 1;

			if (!objects || index >= capacity)
				return 0;

			return *reinterpret_cast<std::uintptr_t*>(
				objects +
				static_cast<std::uintptr_t>(index) * 8);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	std::uint64_t ReadWideBarHandle(std::uintptr_t controller)
	{
		__try
		{
			if (!controller)
				return 0;

			return *reinterpret_cast<std::uint64_t*>(
				controller + kWideBarHandleOffset);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	// 12 words of sub+0x00: owner, cur, target/blend, timer, flag,
	// mode, fill, min, max. snap before+after so "never writes" and
	// "overwritten" dont look the same.
	struct SubBlock
	{
		bool ok = false;
		std::uint32_t word[12] = {};
	};

	// SEH leaf: no unwindable locals, so Drive keeps its ostringstream.
	bool ReadSubBlock(std::uintptr_t sub, SubBlock& out)
	{
		out.ok = false;

		__try
		{
			if (!sub)
				return false;

			const auto words =
				reinterpret_cast<const std::uint32_t*>(sub);

			for (int index = 0; index < 12; ++index)
				out.word[index] = words[index];

			out.ok = true;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out.ok = false;
			return false;
		}
	}

	void AppendWord(std::ostringstream& stream, std::uint32_t word)
	{
		stream
			<< std::hex << std::uppercase
			<< std::setw(8) << std::setfill('0') << word
			<< std::dec << std::setfill(' ');
	}

	void AppendBlock(std::ostringstream& stream, const SubBlock& block)
	{
		if (!block.ok)
		{
			stream << " unreadable";
			return;
		}

		for (int index = 0; index < 12; ++index)
		{
			stream << ' ';
			AppendWord(stream, block.word[index]);
		}
	}

	// Appends "none" or one "+0xNN:old->new" clause per moved word.
	// Returns true when at least one word moved.
	bool AppendDiff(
		std::ostringstream& stream,
		const SubBlock& before,
		const SubBlock& after)
	{
		if (!before.ok || !after.ok)
		{
			stream << " unreadable";
			return false;
		}

		bool moved = false;

		for (int index = 0; index < 12; ++index)
		{
			if (before.word[index] == after.word[index])
				continue;

			moved = true;
			stream << " +0x" << std::hex << std::uppercase
				<< (index * 4) << std::dec << '=';
			AppendWord(stream, before.word[index]);
			stream << "->";
			AppendWord(stream, after.word[index]);
		}

		if (!moved)
			stream << " none";

		return moved;
	}

	// set_value returns without writing - and without any log or fault -
	// when any one of four internal checks fails. Replicated here so the
	// silent drop becomes observable instead of inferred.
	struct SetProbe
	{
		bool uiManager = false;
		bool handleNonZero = false;
		bool indexInRange = false;
		bool objectFound = false;
		bool selfCheck = false;
	};

	bool ProbeSetBail(
		std::uintptr_t moduleBase,
		const std::uint64_t* handlePtr,
		SetProbe& out)
	{
		out.uiManager = false;
		out.handleNonZero = false;
		out.indexInRange = false;
		out.objectFound = false;
		out.selfCheck = false;

		__try
		{
			const auto manager =
				*reinterpret_cast<std::uintptr_t*>(
					moduleBase + kUiManagerRva);

			out.uiManager = (manager != 0);

			if (!manager)
				return false;

			const auto handle = *handlePtr;

			out.handleNonZero = (handle != 0);

			if (!handle)
				return false;

			const auto index =
				static_cast<std::uint32_t>(handle) - 1;

			const auto capacity =
				*reinterpret_cast<std::uint32_t*>(
					manager + kMgrCapacity);

			out.indexInRange = (index < capacity);

			if (index >= capacity)
				return false;

			const auto objects =
				*reinterpret_cast<std::uintptr_t*>(
					manager + kMgrObjects);

			const auto object =
				objects
				? *reinterpret_cast<std::uintptr_t*>(
					objects +
					static_cast<std::uintptr_t>(index) * 8)
				: 0;

			out.objectFound = (object != 0);

			if (!object)
				return false;

			out.selfCheck =
				(*reinterpret_cast<std::uint64_t*>(
					object + kObjHandleOffset) == handle);

			return out.selfCheck;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// called flips true first so a fault still shows how far it got.
	struct DirectResult
	{
		bool vtableOk = false;
		bool slotOk = false;
		bool called = false;
		int faultCode = 0;
	};

	void DispatchDirect(
		std::uintptr_t object,
		float ratio,
		DirectResult& out)
	{
		out.vtableOk = false;
		out.slotOk = false;
		out.called = false;
		out.faultCode = 0;

		__try
		{
			const auto vtable =
				*reinterpret_cast<std::uintptr_t*>(
					object + kObjVtableOffset);

			out.vtableOk = (vtable != 0);

			if (!vtable)
				return;

			const auto slot =
				*reinterpret_cast<std::uintptr_t*>(
					vtable + kSetValueSlotOffset);

			out.slotOk = (slot != 0);

			if (!slot)
				return;

			// Same positional-FP rule as SetValueFn: the handler's
			// `VMOVAPS XMM7, XMM1` reads XMM1, which is where the
			// FIRST float parameter lands.
			using SlotFn = void(__fastcall*)(
				std::uintptr_t object,
				float value);

			out.called = true;

			reinterpret_cast<SlotFn>(slot)(object, ratio);
		}
		__except (
			(out.faultCode = static_cast<int>(GetExceptionCode()),
			 EXCEPTION_EXECUTE_HANDLER))
		{
		}
	}

	// write the fill word + read it back. tells "not writable" apart
	// from "never arrives".
	bool PokeFill(
		std::uintptr_t sub,
		float value,
		float& outReadBack)
	{
		outReadBack = 0.0f;

		__try
		{
			if (!sub)
				return false;

			*reinterpret_cast<float*>(
				sub + kSubCurrentValue) = value;

			outReadBack =
				*reinterpret_cast<float*>(
					sub + kSubCurrentValue);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// default class keeps its value right on the object at +0x2EC,
	// no sub-object needed.
	constexpr std::uintptr_t kObjDefaultValueOffset = 0x2EC;

	bool ReadObjectValue(
		std::uintptr_t object,
		float& outValue)
	{
		outValue = 0.0f;

		__try
		{
			if (!object)
				return false;

			outValue =
				*reinterpret_cast<float*>(
					object + kObjDefaultValueOffset);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool PokeObjectValue(
		std::uintptr_t object,
		float value,
		float& outReadBack)
	{
		outReadBack = 0.0f;

		__try
		{
			if (!object)
				return false;

			*reinterpret_cast<float*>(
				object + kObjDefaultValueOffset) = value;

			outReadBack =
				*reinterpret_cast<float*>(
					object + kObjDefaultValueOffset);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

namespace HJ::HUD::RadialDiscDrive
{
	void Initialize()
	{
		g_moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr));
	}

	bool IsSceneAlive()
	{		// throttled: full pool scan at most twice a second, cached verdict
		// otherwise. the present hook calls this every frame.
		static DWORD lastTick = 0;
		static bool lastAlive = false;
		static std::uintptr_t cached = 0;
		static std::uint64_t cachedHandle = 0;
		if (!g_moduleBase)
			g_moduleBase =
				reinterpret_cast<std::uintptr_t>(
					GetModuleHandleW(nullptr));
		if (!g_moduleBase)
			return false;
		const DWORD now = GetTickCount();
		if (now - lastTick < 500)
			return lastAlive;
		lastTick = now;
		const bool alive = ProbeRootFlag(cached, cachedHandle) >= 0;
		if (alive != lastAlive)
		{
			HJ::Logger::Info(alive ?
				"[RADIAL DISC] life gauge scene alive." :
				"[RADIAL DISC] life gauge scene gone; disc hidden.");
		}
		lastAlive = alive;
		return alive;
	}

	bool IsHudShown()
	{
		// same throttle/cache shape. missing root fails OPEN (keeps the
		// last verdict, true before the first scan) so a scan hiccup can
		// never blank the disc on its own.
		static DWORD lastTick = 0;
		static bool lastShown = true;
		static std::uintptr_t cached = 0;
		static std::uint64_t cachedHandle = 0;
		if (!g_moduleBase)
			g_moduleBase =
				reinterpret_cast<std::uintptr_t>(
					GetModuleHandleW(nullptr));
		if (!g_moduleBase)
			return lastShown;
		const DWORD now = GetTickCount();
		if (now - lastTick < 500)
			return lastShown;
		lastTick = now;
		const int flag = ProbeRootFlag(cached, cachedHandle);
		if (flag < 0)
			return lastShown;
		const bool shown = (static_cast<std::uint32_t>(flag) &
			kHiddenBit) == 0;
		if (shown != lastShown)
		{
			std::ostringstream m;
			m << "[RADIAL DISC] hud " << (shown ? "shown." : "hidden.")
				<< " flag=0x" << std::hex << std::uppercase
				<< static_cast<std::uint32_t>(flag);
			HJ::Logger::Info(m.str());
		}
		lastShown = shown;
		return shown;
	}

	void DumpSceneObjects()
	{
		// snapshot first: collect, then log, so the pool cant shift
		// mid-dump. faults read as 0 (SafeRead), fine for diffing.
		struct Snap { std::uintptr_t obj; std::uint64_t tag; std::uint64_t handle; };
		Snap snaps[4]{};
		int found = 0;
		const auto uiManager = SafeReadPtr(g_moduleBase + kUiManagerRva);
		const auto objects = uiManager ?
			SafeReadPtr(uiManager + kMgrObjects) : 0;
		const auto capacity = uiManager ?
			SafeRead32(uiManager + kMgrCapacity) : 0;
		if (objects && capacity && capacity <= 0x1000000)
		{
			for (std::uint32_t index = 0;
				index < capacity && found < 4; ++index)
			{
				const auto object = SafeReadPtr(
					objects + static_cast<std::uintptr_t>(index) * 8);
				if (!object)
					continue;
				const auto tag = SafeRead64(object + kObjTagOffset);
				if ((tag & 0xFFFFFFFFu) != kSceneLifeGauge)
					continue;
				const auto handle = SafeRead64(object + kObjHandleOffset);
				if (!handle ||
					static_cast<std::uint32_t>(handle) != index + 1)
					continue;
				snaps[found++] = { object, tag, handle };
			}
		}
		{
			std::ostringstream m;
			m << "[RADIAL DUMP] scene objects: " << found;
			HJ::Logger::Info(m.str());
		}
		for (int s = 0; s < found; s++)
		{
			{
				std::ostringstream m;
				m << "[RADIAL DUMP] obj[" << s << "] tag=0x" << std::hex
					<< std::uppercase << snaps[s].tag << " h=0x"
					<< snaps[s].handle;
				HJ::Logger::Info(m.str());
			}
			for (std::uintptr_t off = 0; off < 0x100; off += 0x20)
			{
				std::ostringstream m;
				m << "[RADIAL DUMP] +0x" << std::hex << off;
				for (std::uintptr_t k = 0; k < 0x20; k += 8)
				{
					m << " " << std::setw(16) << std::setfill('0')
						<< SafeRead64(snaps[s].obj + off + k);
				}
				HJ::Logger::Info(m.str());
			}
			{
				// value word + radial child ptr: activity hints.
				const auto v = SafeRead32(snaps[s].obj + 0x2EC);
				const auto c = SafeReadPtr(snaps[s].obj + 0x340);
				std::ostringstream m;
				m << "[RADIAL DUMP] +0x2EC=" << std::hex << std::setw(8)
					<< std::setfill('0') << v << " +0x340=0x" << std::setw(16)
					<< c;
				HJ::Logger::Info(m.str());
			}
		}
	}

	void Drive(float ratio)
	{
		if (!g_moduleBase || !std::isfinite(ratio))
			return;

		if (ratio < 0.0f)
			ratio = 0.0f;

		if (ratio > 1.0f)
			ratio = 1.0f;

		if (g_disabled.load(std::memory_order_relaxed))
			return;

		PollTestKeys();

		const float snap =
			g_testSnap.load(std::memory_order_relaxed);
		if (std::isfinite(snap))
			ratio = snap;

		std::uintptr_t object = 0;
		std::uint64_t handle = 0;

		// Revalidate the cached slot; drop it on tag/handle mismatch or fault.
		// (SEH extracted to a leaf helper: __try + unwindable locals is
		// C2712.)
		if (!ValidateCachedObject(object, handle))
		{
			object = FindRadialObject(&handle);

			if (!object)
				return;

			g_cachedObject = object;

			if (!g_loggedFound.exchange(true))
			{
				const auto vtable =
					*reinterpret_cast<std::uintptr_t*>(
						object + kObjVtableOffset);

				std::ostringstream stream;
				stream
					<< "[RADIAL DISC] layer "
					<< kRadialLayer
					<< " found obj=0x"
					<< std::hex << std::uppercase
					<< object
					<< " handle=0x" << handle
					<< " vtable=0x" << vtable;

				HJ::Logger::Info(stream.str());
			}
		}

		// default class shortcut: value sits at +0x2EC directly, so skip
		// the sub-object path or layers without an interpolator hang.
		if (SafeReadPtr(object + kObjVtableOffset) == kDefaultClassVtable)
		{
			const std::uint32_t driveCount =
				g_driveCount.fetch_add(1, std::memory_order_relaxed);

			float preValue = 0.0f;
			ReadObjectValue(object, preValue);

			SetProbe probe{};
			ProbeSetBail(g_moduleBase, &handle, probe);

			if (!TrySetValue(g_moduleBase + kSetValueRva, &handle, ratio))
			{
				g_disabled.store(true, std::memory_order_relaxed);
				HJ::Logger::Error(
					"[RADIAL DISC] layer 28 set_value faulted; drive "
					"disabled for this session");
				return;
			}

			float postValue = 0.0f;
			ReadObjectValue(object, postValue);
			bool landed = (postValue == ratio);

			bool pokeOk = false;
			float pokeBack = 0.0f;
			if (!landed)
			{
				pokeOk = PokeObjectValue(object, ratio, pokeBack);
				landed = pokeOk && (pokeBack == ratio);
			}

			const bool ratioChanged =
				(ratio != g_lastLoggedRatio.load(std::memory_order_relaxed));
			if (ratioChanged)
				g_lastLoggedRatio.store(ratio, std::memory_order_relaxed);

			const bool failureLine = !landed || pokeOk;
			if (!ratioChanged && !failureLine)
				return;
			if (!ratioChanged &&
				g_logCount.load(std::memory_order_relaxed) >= 64)
				return;

			const std::uint32_t logCount =
				g_logCount.fetch_add(1, std::memory_order_relaxed);

			std::ostringstream stream;
			stream
				<< "[RADIAL DISC] chg #" << logCount
				<< " calls=" << driveCount
				<< " ratio=" << ratio
				<< " obj=0x" << std::hex << std::uppercase << object
				<< std::dec
				<< " probe[ui=" << (probe.uiManager ? 1 : 0)
				<< " h=" << (probe.handleNonZero ? 1 : 0)
				<< " i=" << (probe.indexInRange ? 1 : 0)
				<< " o=" << (probe.objectFound ? 1 : 0)
				<< " s=" << (probe.selfCheck ? 1 : 0) << "]"
				<< " pre2EC=" << preValue
				<< " set2EC=" << postValue;
			if (pokeOk)
				stream << " poke2EC=" << pokeBack;
			stream << " landed=" << (landed ? 1 : 0);

			HJ::Logger::Info(stream.str());
			return;
		}

		// Read the gauge range first, repairing a degenerate one: the
		// handler at 0x142766900 drops the value outright when
		// max <= min, and it does so without any diagnostic of its own.
		// (This also replaces the old unguarded +0x340 read, so a null
		// sub-object is now caught by SEH instead of by luck.)
		SubRange ours{};

		if (!ReadSubRange(object, ours, true))
		{
			if (!g_loggedNoChild.exchange(true))
			{
				HJ::Logger::Warning(
					"[RADIAL DISC] layer 28 radial child (+0x340) is "
					"null; drive deferred");
			}

			return;
		}

		// Always snapshot around set_value: reading 48 bytes twice is
		// nothing, and it means the log can never be blind. A write that
		// does not move any word is the real failure signal, so it keeps
		// logging even after the line cap would otherwise have gone
		// quiet - the previous window expired at 128 calls and hid every
		// damage event that followed.
		const std::uint32_t driveCount =
			g_driveCount.fetch_add(
				1, std::memory_order_relaxed);

		SubBlock before{};
		SubBlock after{};
		SubBlock afterDirect{};

		ReadSubBlock(ours.sub, before);

		SetProbe probe{};
		ProbeSetBail(g_moduleBase, &handle, probe);

		if (!TrySetValue(
				g_moduleBase + kSetValueRva, &handle, ratio))
		{
			g_disabled.store(true, std::memory_order_relaxed);

			HJ::Logger::Error(
				"[RADIAL DISC] layer 28 set_value faulted; drive "
				"disabled for this session");

			return;
		}

		ReadSubBlock(ours.sub, after);

		// Did the value actually arrive? BlocksDiffer alone is NOT the
		// test: when the fill already equals the ratio a correct write
		// changes no word, so "no movement" would read as failure on
		// every healthy call. The transform is exactly (value-0)/(1-0),
		// so after a correct write +0x08 equals ratio bit-for-bit.
		SubRange landedRange{};
		bool landed =
			ReadSubRange(object, landedRange, false) &&
			landedRange.valid &&
			(landedRange.current == ratio);

		DirectResult direct{};
		SubBlock afterPoke{};
		bool pokeOk = false;
		float pokeBack = 0.0f;

		// Always-on fallback: set_value drops the value on any of its
		// four checks without logging, so when it did not arrive we
		// hand the object to the slot directly - identical to
		// set_value's final `MOV RAX,[RCX] / JMP [RAX+0x2C0]`, minus
		// the checks that can discard the value without a trace.
		if (!landed)
		{
			DispatchDirect(object, ratio, direct);
			ReadSubBlock(ours.sub, afterDirect);

			landed =
				afterDirect.ok &&
				(*reinterpret_cast<const float*>(
					ours.sub + kSubCurrentValue) == ratio);

			// Last resort: write the fill word the handler was
			// supposed to produce. If the field is the render source
			// this makes the disc drain regardless of why the engine
			// path declined; if it is not, the next call's `pre8=`
			// says so and the search moves elsewhere.
			if (!landed)
			{
				pokeOk = PokeFill(ours.sub, ratio, pokeBack);
				ReadSubBlock(ours.sub, afterPoke);
			}
		}

		const bool ratioChanged =
			(ratio != g_lastLoggedRatio.load(std::memory_order_relaxed));

		if (ratioChanged)
			g_lastLoggedRatio.store(ratio, std::memory_order_relaxed);

		const bool failureLine =
			!landed || direct.faultCode != 0 || pokeOk;

		// Quiet at steady state. A genuine health change always gets a
		// line - those are rare and are the whole point - while the
		// repetitive "still not landed" chatter keeps a cap so it
		// cannot bury later damage events.
		if (!ratioChanged && !failureLine)
			return;

		if (!ratioChanged &&
			g_logCount.load(std::memory_order_relaxed) >= 64)
			return;

		const std::uint32_t logCount =
			g_logCount.fetch_add(1, std::memory_order_relaxed);

		std::ostringstream stream;
		stream
			<< "[RADIAL DISC] chg #" << logCount
			<< " calls=" << driveCount
			<< " ratio=" << ratio
			<< " sub=0x" << std::hex << std::uppercase << ours.sub
			<< std::dec
			<< " probe[ui=" << (probe.uiManager ? 1 : 0)
			<< " h=" << (probe.handleNonZero ? 1 : 0)
			<< " i=" << (probe.indexInRange ? 1 : 0)
			<< " o=" << (probe.objectFound ? 1 : 0)
			<< " s=" << (probe.selfCheck ? 1 : 0) << "]"
			<< " set_diff:";
		AppendDiff(stream, before, after);

		stream << " | pre8=";
		AppendWord(stream, before.word[2]);
		stream << " set8=";
		AppendWord(stream, after.word[2]);

		if (!direct.called && !direct.vtableOk && !direct.slotOk)
		{
			stream << " | direct=skipped";
		}
		else
		{
			stream << " | direct[v=" << (direct.vtableOk ? 1 : 0)
				<< " s=" << (direct.slotOk ? 1 : 0)
				<< " c=" << (direct.called ? 1 : 0)
				<< " f=0x" << std::hex << direct.faultCode
				<< std::dec << "] dir8=";
			AppendWord(stream, afterDirect.word[2]);
		}

		if (pokeOk)
		{
			stream << " | poke8=";
			AppendWord(stream, afterPoke.word[2]);
		}

		stream << " | ours";
		AppendBlock(
			stream,
			pokeOk ? afterPoke
				: (!landed ? afterDirect : after));

		// On the first line the wide bar the engine drives itself is
		// read alongside. It shares our vtable and is known to drain, so
		// its sub-block is the reference shape the disc should have.
		if (logCount == 0)
		{
			SubRange bar{};

			const auto barObject =
				ResolveHandle(
					ReadWideBarHandle(
						HJ::HUD::RadialHealthBridge::
							GetHealthHudController()));

			if (barObject && ReadSubRange(barObject, bar, false))
			{
				SubBlock barBlock{};
				ReadSubBlock(bar.sub, barBlock);

				stream << " | bar sub=0x" << std::hex
					<< std::uppercase << bar.sub << std::dec;
				AppendBlock(stream, barBlock);
			}
		}

		HJ::Logger::Info(stream.str());
	}
}
