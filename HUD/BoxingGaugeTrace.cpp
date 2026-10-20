#include "BoxingGaugeTrace.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace
{
	SafetyHookMid g_setValueHook{};

	std::uintptr_t g_moduleBase = 0;

	constexpr std::uintptr_t kSetValueRva = 0x027CABE0;

	// .sxdata (0x1000..0x33E2200) + .text1 (0x580B000..0x1AC36040).
	constexpr std::uintptr_t kCodeRvaLo = 0x1000;
	constexpr std::uintptr_t kCodeRvaHi = 0x1AC40000;

	// Known health set_value call sites, used as a tracer control.
	constexpr std::uint32_t kHealthCallerA = 0x0275D3B6;
	constexpr std::uint32_t kHealthCallerB = 0x0275D3CE;


	// QTE radial writers (0x1428A2080): layer lookup by ordinal in r8d,
	// then set_value with (pos-start)/span clamped >= 0.
	struct QteSite
	{
		std::uint32_t callerRva;
		std::uint32_t ordinal;
	};

	constexpr QteSite kQteSites[] =
	{
		{ 0x28A2130, 0x901  },
		{ 0x28A2154, 0x902  },
		{ 0x28A2178, 0x904  },
		{ 0x28A219C, 0x19F7 },
		{ 0x28A21C0, 0x19F8 },
		{ 0x28A21E4, 0x19FA },
	};

	struct QteCapture
	{
		std::uintptr_t object = 0;
		std::uintptr_t vtable = 0;
		std::uintptr_t handler = 0;
		std::uint64_t handle = 0;
		float minValue = 0.0f;
		float maxValue = 0.0f;
		std::uint32_t calls = 0;
		bool valid = false;
	};

	std::array<QteCapture, std::size(kQteSites)> g_qte{};

	bool IsQteSite(
		std::uint32_t callerRva,
		std::size_t& index)
	{
		for (std::size_t i = 0;
			i < std::size(kQteSites);
			++i)
		{
			if (kQteSites[i].callerRva == callerRva)
			{
				index = i;
				return true;
			}
		}

		return false;
	}

	struct TraceEvent
	{
		std::uint32_t callerRva;
		std::uint64_t handle;
		float value;
		std::uint64_t tick;
	};

	constexpr std::size_t kTraceCapacity = 131072;

	std::array<TraceEvent, kTraceCapacity> g_events{};

	std::atomic<std::uint32_t> g_writeIndex{ 0 };

	std::atomic<bool> g_enabled{ false };

	std::atomic<bool> g_f11WasDown{ false };

	std::atomic<bool> g_bufferFull{ false };

	std::atomic<bool> g_qteEnabled{ false };

	struct GroupStats
	{
		std::uint32_t callerRva = 0;
		std::uint64_t handle = 0;
		std::uint64_t count = 0;
		float first = 0.0f;
		float last = 0.0f;
		float minv = 0.0f;
		float maxv = 0.0f;
		std::uint64_t changes = 0;
		std::uint64_t down = 0;
		std::uint64_t up = 0;
		float prevValue = 0.0f;
	};

	bool IsInCodeSection(std::uintptr_t rva)
	{
		return rva >= kCodeRvaLo && rva < kCodeRvaHi;
	}

	const char* DomainName(float minv, float maxv)
	{
		if (minv >= -0.001f && maxv <= 1.001f)
			return "normalized 0..1";
		if (minv >= -0.001f && maxv <= 100.001f)
			return "percent/timeline 0..100";
		return "other";
	}

	std::uintptr_t ResolveObject(std::uint64_t handle)
	{
		if (!handle || !g_moduleBase)
			return 0;

		const auto uiManager =
			*reinterpret_cast<std::uintptr_t*>(
				g_moduleBase + 0x041F0D80
			);

		if (!uiManager)
			return 0;

		const auto capacity =
			*reinterpret_cast<std::uint32_t*>(
				uiManager + 0x19C
			);

		const auto low =
			static_cast<std::uint32_t>(handle);

		if (!low)
			return 0;

		const std::uint32_t index = low - 1;

		if (index >= capacity)
			return 0;

		const auto objects =
			*reinterpret_cast<std::uintptr_t*>(
				uiManager + 0x190
			);

		if (!objects)
			return 0;

		const auto object =
			*reinterpret_cast<std::uintptr_t*>(
				objects +
				static_cast<std::uintptr_t>(index) *
				sizeof(std::uintptr_t)
			);

		if (!object)
			return 0;

		if (
			*reinterpret_cast<std::uint64_t*>(
				object + 0x70
			) != handle)
		{
			return 0;
		}

		return object;
	}

	// Resolve now, while the handle is still live. Doing this at report
	// time loses the object because UI handles get recycled.
	void CaptureQte(
		std::size_t index,
		std::uint64_t handle,
		float value)
	{
		auto& q = g_qte[index];

		q.calls++;

		if (q.calls == 1)
		{
			q.minValue = value;
			q.maxValue = value;
		}
		else
		{
			if (value < q.minValue)
				q.minValue = value;

			if (value > q.maxValue)
				q.maxValue = value;
		}

		if (q.valid)
			return;

		const auto object = ResolveObject(handle);

		if (!object)
			return;

		const auto vtable =
			*reinterpret_cast<std::uintptr_t*>(object);

		q.object = object;
		q.vtable = vtable;
		q.handler =
			*reinterpret_cast<std::uintptr_t*>(
				vtable + 0x2C0);
		q.handle = handle;
		q.valid = true;
	}

	void SetValueHook(SafetyHookContext& context)
	{
		// Reentrancy guard: never record a call made from inside this hook.
		thread_local bool inHook = false;
		if (inHook)
			return;

		if (context.rcx == 0)
			return;

		const auto returnVa =
			*reinterpret_cast<std::uintptr_t*>(context.rsp);

		const auto returnRva =
			returnVa - g_moduleBase;

		if (!IsInCodeSection(returnRva))
			return;

		const auto value =
			context.xmm1.f32[0];

		if (!std::isfinite(value))
			return;

		const auto callerRva =
			static_cast<std::uint32_t>(returnRva) - 5;

		const auto handle =
			*reinterpret_cast<std::uint64_t*>(context.rcx);

		// QTE radial sites are captured independently of the big ring
		// buffer: they only fire during a QTE and they are the payload.
		if (g_qteEnabled.load(std::memory_order_relaxed))
		{
			std::size_t qteIndex = 0;

			if (IsQteSite(callerRva, qteIndex))
			{
				inHook = true;
				CaptureQte(qteIndex, handle, value);
				inHook = false;
			}
		}

		if (!g_enabled.load(std::memory_order_relaxed))
			return;

		const auto index =
			g_writeIndex.fetch_add(
				1,
				std::memory_order_relaxed
			);

		if (index >= kTraceCapacity)
		{
			g_bufferFull.store(true);
			return;
		}

		inHook = true;

		auto& event = g_events[index];
		event.callerRva = callerRva;
		event.handle = handle;
		event.value = value;
		event.tick = GetTickCount64();

		inHook = false;
	}

	void WriteCsv(const std::vector<TraceEvent>& events)
	{
		const char* folder =
			"C:\\Users\\Owner\\Documents\\ModFiles\\"
			"LostJudgmentModIdeas\\trace";

		CreateDirectoryA(folder, nullptr);

		const std::string path =
			std::string(folder) + "\\boxing_ui_trace.csv";

		std::ofstream csv(path, std::ios::out | std::ios::trunc);

		if (!csv.is_open())
			return;

		csv << "caller_rva,handle,value,tick\n";

		for (const auto& e : events)
		{
			csv
				<< std::hex << std::uppercase
				<< "0x" << e.callerRva << ','
				<< "0x" << e.handle << ','
				<< std::dec << e.value << ','
				<< e.tick << '\n';
		}
	}

	void AnalyzeAndReport()
	{
		const std::uint32_t recorded =
			g_writeIndex.load(std::memory_order_relaxed);

		const std::uint32_t count =
			recorded > kTraceCapacity
			? kTraceCapacity
			: recorded;

		std::ostringstream head;
		head
			<< "[BOXING UI TRACE] stopped. events="
			<< count
			<< (g_bufferFull.load() ? " (buffer FULL, later events dropped)" : "");

		HJ::Logger::Info(head.str());

		if (count == 0)
		{
			HJ::Logger::Info(
				"[BOXING UI TRACE] no events captured. "
				"Were you in boxing when you pressed F11?"
			);
			return;
		}

		std::vector<TraceEvent> events(
			g_events.begin(),
			g_events.begin() + count
		);

		WriteCsv(events);

		std::sort(
			events.begin(),
			events.end(),
			[](const TraceEvent& a, const TraceEvent& b)
			{
				if (a.callerRva != b.callerRva)
					return a.callerRva < b.callerRva;
				return a.handle < b.handle;
			}
		);

		std::vector<GroupStats> groups;

		for (const auto& e : events)
		{
			if (!groups.empty() &&
				groups.back().callerRva == e.callerRva &&
				groups.back().handle == e.handle)
			{
				auto& g = groups.back();
				g.count++;
				g.last = e.value;
				if (e.value < g.minv) g.minv = e.value;
				if (e.value > g.maxv) g.maxv = e.value;
				if (g.count > 1)
				{
					const float delta = e.value - g.prevValue;
					if (std::fabs(delta) > 0.0005f)
					{
						g.changes++;
						if (delta < 0.0f) g.down++;
						else g.up++;
					}
				}
				g.prevValue = e.value;
			}
			else
			{
				GroupStats g{};
				g.callerRva = e.callerRva;
				g.handle = e.handle;
				g.count = 1;
				g.first = e.value;
				g.last = e.value;
				g.minv = e.value;
				g.maxv = e.value;
				g.prevValue = e.value;
				groups.push_back(g);
			}
		}

		// Rank by meaningful movement: changes * range.
		std::sort(
			groups.begin(),
			groups.end(),
			[](const GroupStats& a, const GroupStats& b)
			{
				const float ar = a.maxv - a.minv;
				const float br = b.maxv - b.minv;
				const float as =
					static_cast<float>(a.changes) * ar;
				const float bs =
					static_cast<float>(b.changes) * br;
				return as > bs;
			}
		);

		const std::size_t topCount =
			groups.size() < 30 ? groups.size() : 30;

		for (std::size_t i = 0; i < topCount; ++i)
		{
			const auto& g = groups[i];

			const float range = g.maxv - g.minv;

			std::ostringstream line;
			line
				<< "[BOXING UI TRACE] candidate #" << (i + 1)
				<< " caller_rva=0x" << std::hex << std::uppercase
				<< g.callerRva
				<< " handle=0x" << g.handle
				<< " calls=" << std::dec << g.count
				<< " first=" << g.first
				<< " last=" << g.last
				<< " min=" << g.minv
				<< " max=" << g.maxv
				<< " range=" << range
				<< " changes=" << g.changes
				<< " down=" << g.down
				<< " up=" << g.up
				<< " domain=" << DomainName(g.minv, g.maxv);

			if (g.callerRva == kHealthCallerA ||
				g.callerRva == kHealthCallerB)
				line << " [health-hp control]";

			HJ::Logger::Info(line.str());
		}

		// Resolve handles for the strongest candidates (off the hot path).
		const std::size_t resolveCount =
			groups.size() < 8 ? groups.size() : 8;

		for (std::size_t i = 0; i < resolveCount; ++i)
		{
			const auto& g = groups[i];
			const auto object = ResolveObject(g.handle);

			std::ostringstream line;
			line
				<< "[BOXING UI TRACE] resolve #" << (i + 1)
				<< " caller_rva=0x" << std::hex << std::uppercase
				<< g.callerRva
				<< " handle=0x" << g.handle
				<< " object=0x" << object;

			if (object)
			{
				const auto vtable =
					*reinterpret_cast<std::uintptr_t*>(object);

				line
					<< " vtable=0x" << vtable
					<< " (image+0x"
					<< (vtable - g_moduleBase)
					<< ")";
			}
			else
			{
				line << " object=INVALID (handle may be recycled)";
			}

			HJ::Logger::Info(line.str());
		}

		HJ::Logger::Info(
			"[BOXING UI TRACE] csv: "
			"C:\\Users\\Owner\\Documents\\ModFiles\\"
			"LostJudgmentModIdeas\\trace\\boxing_ui_trace.csv"
		);
	}

	void ReportQte()
	{
		std::ostringstream head;

		head
			<< "[QTE RADIAL] sites captured: ";

		std::size_t hit = 0;

		for (const auto& q : g_qte)
		{
			if (q.calls)
				hit++;
		}

		head << hit << " / " << std::size(kQteSites);

		HJ::Logger::Info(head.str());

		for (std::size_t i = 0;
			i < std::size(kQteSites);
			++i)
		{
			const auto& q = g_qte[i];

			if (!q.calls)
				continue;

			std::ostringstream line;

			line
				<< "[QTE RADIAL] ordinal=0x" << std::hex
				<< std::uppercase << kQteSites[i].ordinal
				<< std::dec
				<< " caller_rva=0x" << std::hex << std::uppercase
				<< kQteSites[i].callerRva
				<< std::dec
				<< " calls=" << q.calls
				<< " min=" << q.minValue
				<< " max=" << q.maxValue;

			if (q.valid)
			{
				line
					<< " object=0x" << std::hex << std::uppercase
					<< q.object
					<< " vtable=0x" << q.vtable
					<< " (image+0x"
					<< (q.vtable - g_moduleBase)
					<< ")"
					<< " +0x2C0=0x" << q.handler
					<< " (image+0x"
					<< (q.handler - g_moduleBase)
					<< ")";
			}
			else
			{
				line << " object=UNRESOLVED";
			}

			HJ::Logger::Info(line.str());
		}

		HJ::Logger::Info(
			"[QTE RADIAL] compare against health: "
			"+178/+180 vtable 0x1435ED368, "
			"+190/+198 vtable 0x1435EE198 (no-op at +0x2C0)"
		);
	}

	DWORD WINAPI TraceHotkeyThread(LPVOID)
	{
		for (;;)
		{
			const bool down =
				(GetAsyncKeyState(VK_F11) & 0x8000) != 0;

			const bool wasDown =
				g_f11WasDown.exchange(down);

			if (down && !wasDown)
			{
				if (!g_enabled.load(std::memory_order_relaxed))
				{
					g_writeIndex.store(
						0,
						std::memory_order_relaxed
					);

					g_bufferFull.store(false);

					for (auto& q : g_qte)
						q = QteCapture{};

					g_qteEnabled.store(
						true,
						std::memory_order_release
					);

					g_enabled.store(
						true,
						std::memory_order_release
					);

					HJ::Logger::Info(
						"[BOXING UI TRACE] started (F11 to stop)"
					);
				}
				else
				{
					g_enabled.store(
						false,
						std::memory_order_release
					);

					g_qteEnabled.store(
						false,
						std::memory_order_release
					);

					// Let any in-flight hook write finish.
					Sleep(100);

					AnalyzeAndReport();
					ReportQte();
				}
			}

			Sleep(10);
		}

		return 0;
	}
}

namespace HJ::HUD::BoxingGaugeTrace
{
	bool Initialize()
	{
		g_moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr)
			);

		if (!g_moduleBase)
		{
			HJ::Logger::Error(
				"[BOXING UI TRACE] no game module"
			);

			return false;
		}

		const auto target =
			g_moduleBase + kSetValueRva;

		const auto* bytes =
			reinterpret_cast<const std::uint8_t*>(target);

		// mov [rsp+8],rbx ; mov [rsp+0x10],rbp ;
		// mov [rsp+0x18],rsi ; push rdi
		constexpr std::uint8_t kExpected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08,
			0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18,
			0x57
		};

		if (std::memcmp(bytes, kExpected, sizeof(kExpected)) != 0)
		{
			HJ::Logger::Error(
				"[BOXING UI TRACE] set_value address didnt match"
			);

			return false;
		}

		g_setValueHook =
			safetyhook::create_mid(
				reinterpret_cast<void*>(target),
				&SetValueHook
			);

		if (!g_setValueHook)
		{
			HJ::Logger::Error(
				"[BOXING UI TRACE] set_value hook failed"
			);

			return false;
		}

		const auto thread =
			CreateThread(
				nullptr,
				0,
				&TraceHotkeyThread,
				nullptr,
				0,
				nullptr
			);

		if (!thread)
		{
			HJ::Logger::Error(
				"[BOXING UI TRACE] hotkey thread failed"
			);

			return false;
		}

		CloseHandle(thread);

		HJ::Logger::Info(
			"[BOXING UI TRACE] ready (F11 toggles; off by default)"
		);

		return true;
	}

	void Shutdown()
	{
		g_enabled.store(false, std::memory_order_release);
	}
}
