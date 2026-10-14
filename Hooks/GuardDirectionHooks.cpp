#include "GuardDirectionHooks.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>
#include <safetyhook.hpp>
#include <Xinput.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <intrin.h>

extern "C" std::uintptr_t HJ_GetPlayerCombatObject();

namespace
{
	SafetyHookInline g_guardGateHook{};
	SafetyHookInline g_guardGateHook2{};
	SafetyHookInline g_liveDispatchHook{};
	SafetyHookInline g_dispatchHook{};
	SafetyHookInline g_resolveHook{};
	SafetyHookInline g_guardAllowedHook{};

	constexpr std::uintptr_t kGuardGateRva = 0x2282AC0;
	constexpr std::uintptr_t kGuardGate2Rva = 0x22758C0;
	constexpr std::uintptr_t kLiveDispatchRva = 0x2EF15A0;
	constexpr std::uintptr_t kDispatchRva = 0x2EE4CA0;
	constexpr std::uintptr_t kResolveRva = 0x4847CA60;
	constexpr std::uintptr_t kGuardAllowedRva = 0x2E97660;
	constexpr std::uintptr_t kAngleRva = 0x2E84090;
	constexpr std::uintptr_t kSlotResolveRva = 0x2ED1270;

	// twins silent on player path (F6 trace), deny lives on the live site now
	constexpr bool kHookGates = false;
	constexpr bool kHookLiveDispatch = true;
	constexpr bool kHookDispatch = true;
	constexpr bool kHookAllowed = false;
	
	constexpr bool kHookResolve = false;

	constexpr float kCone = 1.047f;
	constexpr float kSideCone = 1.92f;
	constexpr float kFrontOverlap = 0.1f;
	constexpr int kNeutralBudget = 1;
	constexpr int kSideBudget = 3;

	constexpr std::uint32_t kLeftSt = 16319;
	constexpr std::uint32_t kLeftLp = 16317;
	constexpr std::uint32_t kLeftEn = 16315;

	constexpr std::uint32_t kRightSt = 16328;
	constexpr std::uint32_t kRightLp = 16321;
	constexpr std::uint32_t kRightEn = 16320;

	// deployed mod table neutral triple (E_GRD normal guard)
	constexpr std::uint16_t kModSt = 5559;
	constexpr std::uint16_t kModLp = 13919;
	constexpr std::uint16_t kModEn = 300;

	
	constexpr std::uint16_t kVanSt = 15187;
	constexpr std::uint16_t kVanLp = 15186;
	constexpr std::uint16_t kVanEn = 15180;


	constexpr std::uint8_t kGuardRowSig[] = {
		0x6F, 0x3B, 0xA1, 0x3B, 0xA2, 0x3B, 0xA3, 0x3B,
		0xB7, 0x15, 0x5F, 0x36, 0x2C, 0x01
	};

	
	constexpr std::uint8_t kGuardRowSigVan[] = {
		0x6F, 0x3B, 0x6C, 0x3B, 0x72, 0x3B, 0x73, 0x3B,
		0x53, 0x3B, 0x52, 0x3B, 0x4C, 0x3B
	};

	constexpr std::size_t kGuardRowSigLen = 14;

	
	constexpr std::uint16_t kBoxerSway[] = {
		15215, 15265, 15266, 15267
	};

	// deployed mod row 283 content for row recognition (u16 values)
	constexpr std::uint16_t kRowNeedles[] = {
		15215, 15265, 15266, 15267, 5559, 13919, 300, 9280, 899,
		15205, 15204, 15206, 15207, 15209, 15208, 15210, 15211,
		1366, 97, 9694, 15216, 15217, 15213, 15214
	};

	
	std::atomic<std::uint16_t> g_neutralSt{ kModSt };
	std::atomic<std::uint16_t> g_neutralLp{ kModLp };
	std::atomic<std::uint16_t> g_neutralEn{ kModEn };

	std::atomic<std::uintptr_t> g_guardStCell{ 0 };
	std::atomic<std::uintptr_t> g_guardLpCell{ 0 };
	std::atomic<std::uintptr_t> g_guardEnCell{ 0 };
	std::atomic<int> g_lastAppliedSide{ -2 }; // -2 unknown, -1 vanilla, 0 both, 1 left, 2 right

	bool SafeReadBytes(std::uintptr_t address, void* out, std::size_t size) noexcept
	{
		__try
		{
			std::memcpy(out, reinterpret_cast<const void*>(address), size);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	
	struct ScanRegion
	{
		std::uintptr_t base;
		std::size_t size;
	};

	int CompareRegionSize(const void* a, const void* b) noexcept
	{
		const std::size_t sa = static_cast<const ScanRegion*>(a)->size;
		const std::size_t sb = static_cast<const ScanRegion*>(b)->size;
		if (sa < sb)
			return -1;
		if (sa > sb)
			return 1;
		return 0;
	}

	bool ScanOverBudget(const LARGE_INTEGER& freq, const LARGE_INTEGER& start) noexcept
	{
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		return freq.QuadPart != 0 &&
			(now.QuadPart - start.QuadPart) * 1000 / freq.QuadPart > 10000;
	}

	int ScanGuardRow(std::uintptr_t* outCands, int candCap, const std::uint8_t* sig, bool includeImage, bool includeReadOnly, SIZE_T* outWalkedMB, int* outDropped) noexcept
	{
		SYSTEM_INFO sys{};
		GetSystemInfo(&sys);

		LARGE_INTEGER freq{};
		QueryPerformanceFrequency(&freq);
		LARGE_INTEGER start{};
		QueryPerformanceCounter(&start);

		// collect first, touching nothing, then smallest first
		constexpr int kMaxRegions = 65536;
		static ScanRegion regions[kMaxRegions];
		int nregions = 0;
		int dropped = 0;

		std::uintptr_t addr = 0;
		MEMORY_BASIC_INFORMATION mbi{};
		while (addr < reinterpret_cast<std::uintptr_t>(sys.lpMaximumApplicationAddress))
		{
			if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
				break;

			const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			addr = base + mbi.RegionSize;

			if (mbi.State != MEM_COMMIT)
				continue;

			if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
				continue;

			if (mbi.Type != MEM_PRIVATE)
			{
				if (!includeImage || mbi.Type != MEM_IMAGE)
					continue;
			}

			const DWORD prot = mbi.Protect & 0xFF;
			const bool writable =
				prot == PAGE_READWRITE || prot == PAGE_WRITECOPY;
			if (!writable && !(includeReadOnly &&
				(prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ)))
				continue;

			if (nregions < kMaxRegions)
			{
				regions[nregions].base = base;
				regions[nregions].size = mbi.RegionSize;
				++nregions;
			}
			else
			{
				++dropped;
			}
		}

		qsort(regions, nregions, sizeof(regions[0]), CompareRegionSize);

		int matches = 0;
		SIZE_T walkedMB = 0;

		for (int i = 0; i < nregions; ++i)
		{
			if (ScanOverBudget(freq, start))
				break;

			const std::uintptr_t base = regions[i].base;
			walkedMB += regions[i].size >> 20;
			bool timedOut = false;

			__try
			{
				const auto* begin = reinterpret_cast<const std::uint8_t*>(base);
				const auto* regionEnd = begin + regions[i].size;
				const auto* p = begin;
				unsigned chunk = 0;

				
				while (p + kGuardRowSigLen <= regionEnd)
				{
					p = reinterpret_cast<const std::uint8_t*>(
						std::memchr(p, sig[0],
							static_cast<std::size_t>(regionEnd - p)));

					if (p == nullptr)
						break;

					if (p + kGuardRowSigLen <= regionEnd &&
						std::memcmp(p, sig, kGuardRowSigLen) == 0)
					{
						if (matches < candCap)
							outCands[matches] = reinterpret_cast<std::uintptr_t>(p + 8);
						++matches;
					}

					++p;

					
					if ((++chunk & 1023) == 0 && ScanOverBudget(freq, start))
					{
						timedOut = true;
						break;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				continue;
			}

			if (timedOut)
				break;
		}

		*outWalkedMB = walkedMB;
		*outDropped = dropped;
		return matches;
	}

	
	bool FindGuardRow()
	{
		return g_guardLpCell.load(std::memory_order_acquire) != 0;
	}

	bool SafeReadU16(std::uintptr_t address, std::uint16_t& out) noexcept
	{
		__try
		{
			out = *reinterpret_cast<const std::uint16_t*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	
	int ScanHeaps(std::uintptr_t* outCands, int candCap, const std::uint8_t* sig, SIZE_T* outMB) noexcept
	{
		int matches = 0;
		SIZE_T mb = 0;

		__try
		{
			HANDLE heaps[64];
			const DWORD nheaps = GetProcessHeaps(64, heaps);

			for (DWORD h = 0; h < nheaps; ++h)
			{
				const HANDLE heap = heaps[h];
				if (heap == nullptr)
					continue;

				HeapLock(heap);

				PROCESS_HEAP_ENTRY entry{};
				while (HeapWalk(heap, &entry))
				{
					if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0)
						continue;

					const auto* begin =
						reinterpret_cast<const std::uint8_t*>(entry.lpData);
					const SIZE_T size = entry.cbData;
					if (begin == nullptr || size < kGuardRowSigLen)
						continue;

					mb += size >> 20;

					const auto* regionEnd = begin + size;
					const auto* p = begin;

					while (p + kGuardRowSigLen <= regionEnd)
					{
						p = reinterpret_cast<const std::uint8_t*>(
							std::memchr(p, sig[0],
								static_cast<std::size_t>(regionEnd - p)));

						if (p == nullptr)
							break;

						if (p + kGuardRowSigLen <= regionEnd &&
							std::memcmp(p, sig, kGuardRowSigLen) == 0)
						{
							if (matches < candCap)
								outCands[matches] =
									reinterpret_cast<std::uintptr_t>(p + 8);
							++matches;
						}

						++p;
					}
				}

				HeapUnlock(heap);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		*outMB = mb;
		return matches;
	}

	
	void ScanBoxerRow(std::uintptr_t* outLp, int* outSway, std::uint16_t* outVal, int* outAnchors) noexcept
	{
		const std::uint16_t anchors[3] = { kLeftLp, kModLp, kVanLp };
		std::uintptr_t bestLp = 0;
		int bestSway = -1;
		std::uint16_t bestVal = 0;
		int anchorHits = 0;

		__try
		{
			HANDLE heaps[64];
			const DWORD nheaps = GetProcessHeaps(64, heaps);

			for (DWORD h = 0; h < nheaps; ++h)
			{
				const HANDLE heap = heaps[h];
				if (heap == nullptr)
					continue;

				HeapLock(heap);

				PROCESS_HEAP_ENTRY entry{};
				while (HeapWalk(heap, &entry))
				{
					if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0)
						continue;

					const auto* begin =
						reinterpret_cast<const std::uint8_t*>(entry.lpData);
					const SIZE_T size = entry.cbData;
					if (begin == nullptr || size < 2)
						continue;

					const auto* regionEnd = begin + size;

					for (int a = 0; a < 3; ++a)
					{
						const std::uint8_t lo =
							static_cast<std::uint8_t>(anchors[a] & 0xFF);
						const std::uint8_t hi =
							static_cast<std::uint8_t>(anchors[a] >> 8);
						const auto* p = begin;

						while (p + 2 <= regionEnd)
						{
							p = reinterpret_cast<const std::uint8_t*>(
								std::memchr(p, lo,
									static_cast<std::size_t>(regionEnd - p)));
							if (p == nullptr)
								break;
							if (p + 2 > regionEnd || p[1] != hi ||
								(reinterpret_cast<std::uintptr_t>(p) & 1) != 0)
							{
								++p;
								continue;
							}

							++anchorHits;
							const std::uintptr_t hit =
								reinterpret_cast<std::uintptr_t>(p);

							int sway = 0;
							for (int w = -16; w <= 16; ++w)
							{
								std::uint16_t v = 0;
								const std::uintptr_t at =
									hit + static_cast<std::uint64_t>(
										static_cast<std::int64_t>(w) * 2);
								if (!SafeReadU16(at, v))
									continue;
								for (int s = 0; s < 4; ++s)
								{
									if (v == kBoxerSway[s])
									{
										++sway;
										break;
									}
								}
							}

							if (sway > bestSway)
							{
								bestSway = sway;
								bestLp = hit;
								bestVal = anchors[a];
							}

							++p;
						}
					}
				}

				HeapUnlock(heap);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		*outLp = bestLp;
		*outSway = bestSway;
		*outVal = bestVal;
		*outAnchors = anchorHits;
	}

	// a candidate only wins if it actually holds the expected neutral triple
	bool VerifyTriple(std::uintptr_t stCell, std::uint16_t st, std::uint16_t lp, std::uint16_t en)
	{
		std::uint16_t hasSt = 0;
		std::uint16_t hasLp = 0;
		std::uint16_t hasEn = 0;

		return SafeReadU16(stCell, hasSt) &&
			SafeReadU16(stCell + 2, hasLp) &&
			SafeReadU16(stCell + 4, hasEn) &&
			hasSt == st &&
			hasLp == lp &&
			hasEn == en;
	}

	
	void LogAnchorContexts(const std::uint8_t* begin, SIZE_T size,
		const std::uint8_t needle[2], const char* tag, int& hits, int cap)
	{
		if (begin == nullptr || size < 2 || hits >= cap)
			return;

		const auto* regionEnd = begin + size;
		const auto* p = begin;

		while (hits < cap && p + 2 <= regionEnd)
		{
			p = reinterpret_cast<const std::uint8_t*>(
				std::memchr(p, needle[0],
					static_cast<std::size_t>(regionEnd - p)));

			if (p == nullptr)
				break;

			if (p + 2 <= regionEnd && p[1] == needle[1] &&
				(reinterpret_cast<std::uintptr_t>(p) & 1) == 0)
			{
				const auto* from = p >= begin + 48 ? p - 48 : begin;
				const auto* to = p + 64 <= regionEnd ? p + 64 : regionEnd;

				std::string hex;
				hex.reserve(static_cast<std::size_t>(to - from) * 2);
				for (const auto* q = from; q < to; ++q)
					hex += std::format("{:02X}", *q);

				HJ::Logger::Info(std::format(
					"HJ GUARD: anchor {} @0x{:X} off={} ctx={}",
					tag, reinterpret_cast<std::uintptr_t>(p),
					static_cast<int>(p - from), hex));
				++hits;
			}

			++p;
		}
	}

	// POD-only heap walk: __try is legal here, no C++ objects inside.
	// logging lives in the caller, never inside the __try (C2712).
	void DumpHeapAnchors(const std::uint8_t* modNeedle, const std::uint8_t* vanNeedle,
		int cap, int& modHits, int& vanHits) noexcept
	{
		__try
		{
			HANDLE heaps[64];
			const DWORD nheaps = GetProcessHeaps(64, heaps);

			for (DWORD h = 0;
				h < nheaps && (modHits < cap || vanHits < cap);
				++h)
			{
				const HANDLE heap = heaps[h];
				if (heap == nullptr)
					continue;

				HeapLock(heap);

				PROCESS_HEAP_ENTRY entry{};
				while (HeapWalk(heap, &entry) &&
					(modHits < cap || vanHits < cap))
				{
					if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0)
						continue;

					const auto* begin =
						reinterpret_cast<const std::uint8_t*>(entry.lpData);
					const SIZE_T size = entry.cbData;

					LogAnchorContexts(begin, size, modNeedle, "mod", modHits, cap);
					LogAnchorContexts(begin, size, vanNeedle, "van", vanHits, cap);
				}

				HeapUnlock(heap);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	void GuardRowScanThread()
	{
		HJ::Logger::Info("HJ GUARD: boxer row hunt started");

		std::uintptr_t lp = 0;
		int sway = -1;
		std::uint16_t val = 0;
		int anchors = 0;
		ScanBoxerRow(&lp, &sway, &val, &anchors);

		HJ::Logger::Info(std::format(
			"HJ GUARD: boxer hunt done, anchors={} bestSway={}",
			anchors, sway));

		// override any mirror adoption with the style-fingerprinted row
		if (lp != 0 && sway >= 3)
		{
			std::uint16_t cur = 0;
			if (SafeReadU16(lp, cur) && cur == val)
			{
				g_neutralLp.store(cur, std::memory_order_release);
				g_guardLpCell.store(lp, std::memory_order_release);
				g_lastAppliedSide.store(-2, std::memory_order_release);

				HJ::Logger::Info(std::format(
					"HJ GUARD: row via heap boxer lp=0x{:X} val={} sway={} (override)",
					lp, cur, sway));
			}
		}
	}

	void EnsureScanThread()
	{
		static std::once_flag flag;
		std::call_once(flag, [] { std::thread(GuardRowScanThread).detach(); });
	}

	bool WriteU16Cell(std::uintptr_t cell, std::uint16_t value) noexcept
	{
		__try
		{
			DWORD old = 0;
			if (!VirtualProtect(reinterpret_cast<LPVOID>(cell), sizeof(value),
				PAGE_EXECUTE_READWRITE, &old))
				return false;

			*reinterpret_cast<volatile std::uint16_t*>(cell) = value;

			DWORD tmp = 0;
			VirtualProtect(reinterpret_cast<LPVOID>(cell), sizeof(value), old, &tmp);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// side: -1 vanilla, 0 both, 1 left, 2 right. writes only on change
	void ApplyGuardTriple(int side) noexcept
	{
		if (side == g_lastAppliedSide.load(std::memory_order_acquire))
			return;

		if (!FindGuardRow())
			return;

		std::uint16_t st = g_neutralSt.load(std::memory_order_acquire);
		std::uint16_t lp = g_neutralLp.load(std::memory_order_acquire);
		std::uint16_t en = g_neutralEn.load(std::memory_order_acquire);

		if (side == 1)
		{
			st = static_cast<std::uint16_t>(kLeftSt);
			lp = static_cast<std::uint16_t>(kLeftLp);
			en = static_cast<std::uint16_t>(kLeftEn);
		}
		else if (side == 2)
		{
			st = static_cast<std::uint16_t>(kRightSt);
			lp = static_cast<std::uint16_t>(kRightLp);
			en = static_cast<std::uint16_t>(kRightEn);
		}

		const std::uintptr_t stCell = g_guardStCell.load(std::memory_order_acquire);
		const std::uintptr_t lpCell = g_guardLpCell.load(std::memory_order_acquire);
		const std::uintptr_t enCell = g_guardEnCell.load(std::memory_order_acquire);

		// unknown cells stay zero with lp-only adoption, skip those
		auto writeCell = [&](std::uintptr_t cell, std::uint16_t want) -> bool
		{
			if (cell == 0)
				return true;
			if (!WriteU16Cell(cell, want))
				return false;
			std::uint16_t have = 0;
			return SafeReadU16(cell, have) && have == want;
		};

		if (!writeCell(stCell, st) ||
			!writeCell(lpCell, lp) ||
			!writeCell(enCell, en))
		{
			HJ::Logger::Info("HJ GUARD: cell write failed");
			return;
		}

		g_lastAppliedSide.store(side, std::memory_order_release);

		HJ::Logger::Info(std::format(
			"HJ GUARD: triple -> {} (st={} lp={} en={})",
			side == -1 ? "vanilla" :
			side == 0 ? "both" :
			side == 1 ? "left" : "right",
			st, lp, en));
	}

	enum class GuardSide : std::uint8_t
	{
		None,
		Both,
		Left,
		Right
	};

	std::atomic<int> g_bothBlocks{ 0 };
	std::atomic<int> g_holdSide{ -2 };
	std::atomic<int> g_blockCount{ 0 };
	std::atomic<unsigned> g_ownerMismatchCount{ 0 };
	std::atomic<unsigned> g_angleFailCount{ 0 };

	using SlotResolveFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
	using AngleFn = float*(__fastcall*)(std::uint64_t, float*, void*, void*);

	bool SafeReadU32(std::uintptr_t address, std::uint32_t& out) noexcept
	{
		__try
		{
			out = *reinterpret_cast<const std::uint32_t*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	GuardSide GetGuardSide() noexcept
	{
		XINPUT_STATE state{};

		if (XInputGetState(0, &state) != ERROR_SUCCESS)
			return GuardSide::None;

		// Circle alone = neutral, Circle+LB = left, Circle+RB = right
		const bool b = (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
		if (!b)
			return GuardSide::None;

		const bool lb = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
		const bool rb = (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;

		if (lb && !rb)
			return GuardSide::Left;

		if (rb && !lb)
			return GuardSide::Right;

		return GuardSide::Both;
	}

	bool TryGetBlockAngle(std::uintptr_t owner, std::uintptr_t packet, float& outAngle) noexcept
	{
		const auto base =
			reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

		if (base == 0)
			return false;

		auto slotResolve = reinterpret_cast<SlotResolveFn>(base + kSlotResolveRva);
		auto angleFn = reinterpret_cast<AngleFn>(base + kAngleRva);

		if (!slotResolve || !angleFn)
			return false;

		std::uint32_t attacker = 0;
		if (!SafeReadU32(packet + 0x58, attacker) || attacker == 0)
			return false;

		__try
		{
			const std::uintptr_t context = slotResolve(owner + 0x30);
			if (context == 0)
				return false;

			const std::uintptr_t entity = *reinterpret_cast<const std::uintptr_t*>(context);
			if (entity == 0)
				return false;

			const std::uint32_t defender = *reinterpret_cast<const std::uint32_t*>(entity + 0xC);
			if (defender == 0)
				return false;

			std::uint32_t atk[2] = { attacker, 0 };
			std::uint64_t defBlob = static_cast<std::uint64_t>(defender);

			float angle = 0.0f;
			angleFn(defender, &angle, &defBlob, atk);
			if (!std::isfinite(angle))
				return false;

			outAngle = angle;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool SafeReadPtr(std::uintptr_t address, std::uintptr_t& out) noexcept
	{
		__try
		{
			out = *reinterpret_cast<const std::uintptr_t*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool ReadU16Block(std::uintptr_t base, std::uint16_t* out, int count) noexcept
	{
		__try
		{
			const auto* src = reinterpret_cast<const std::uint16_t*>(base);
			for (int i = 0; i < count; ++i)
				out[i] = src[i];
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	using GuardOrigFn = std::uint8_t(__fastcall*)(std::uintptr_t, std::uintptr_t);

	std::uint8_t CallGuardOrig1(std::uintptr_t owner, std::uintptr_t packet)
	{
		return g_guardGateHook.call<std::uint8_t>(owner, packet);
	}

	std::uint8_t CallGuardOrig2(std::uintptr_t owner, std::uintptr_t packet)
	{
		return g_guardGateHook2.call<std::uint8_t>(owner, packet);
	}

	// shared allow/deny for both guard gates, orig selects the right
	// trampoline per entry point, gate tags which path fired in logs
	std::uint8_t GuardGateImpl(std::uintptr_t owner, std::uintptr_t packet,
		GuardOrigFn orig, const char* gate)
	{
		// allow/deny only, the per-frame path owns the motion_set row
		const GuardSide side = GetGuardSide();

		const int sideId = side == GuardSide::None ? -1 : side == GuardSide::Both ? 0 : side == GuardSide::Left ? 1 : 2;
		if (sideId != g_holdSide.exchange(sideId))
			g_blockCount.store(0);

		if (side == GuardSide::None)
			return orig(owner, packet);

		// directional logic is player-only, everyone else keeps vanilla
		const std::uintptr_t playerObject = HJ_GetPlayerCombatObject();
		if (playerObject == 0 || owner != playerObject)
		{
			const unsigned n = g_ownerMismatchCount.fetch_add(1) + 1;
			if (n <= 3 || n % 500 == 0)
			{
				HJ::Logger::Info(std::format(
					"HJ GUARD: {} non-player owner=0x{:X} player=0x{:X} (#{})",
					gate, owner, playerObject, n));
			}
			return orig(owner, packet);
		}

		float angle = 0.0f;
		if (!TryGetBlockAngle(owner, packet, angle))
		{
			// never silent: exposes offset and packet-layout problems
			const unsigned n = g_angleFailCount.fetch_add(1) + 1;
			if (n <= 5 || n % 200 == 0)
			{
				HJ::Logger::Info(std::format(
					"HJ GUARD: {} angle resolve failed #{} owner=0x{:X} packet=0x{:X}",
					gate, n, owner, packet));
			}
			return orig(owner, packet);
		}

		const float absAngle = std::fabs(angle);

		if (side == GuardSide::Both)
		{
			if (absAngle > kCone)
			{
				HJ::Logger::Info(std::format("HJ GUARD: {} deny both cone angle={:.3f}", gate, angle));
				return 0;
			}

			if (g_blockCount.load() >= kNeutralBudget)
			{
				HJ::Logger::Info(std::format("HJ GUARD: {} guard broken", gate));
				return 0;
			}

			const std::uint8_t result = orig(owner, packet);
			if (result != 0)
			{
				// neutral holds both sides but pops open right after, real break queued later
				const int n = g_bothBlocks.fetch_add(1) + 1;
				g_blockCount.fetch_add(1);
				HJ::Logger::Info(std::format("HJ GUARD: {} neutral blocked angle={:.3f} count={}", gate, angle, n));
			}
			return result;
		}

		const bool wantLeft = side == GuardSide::Left;

		const int budget = side == GuardSide::Both ? kNeutralBudget : kSideBudget;
		if (g_blockCount.load() >= budget)
		{
			HJ::Logger::Info(std::format("HJ GUARD: {} guard broken", gate));
			return 0;
		}

		if (absAngle <= kFrontOverlap)
		{
			const std::uint8_t frontResult = orig(owner, packet);
			if (frontResult != 0)
			{
				g_blockCount.fetch_add(1);
				std::uint32_t motion = wantLeft ? kLeftLp : kRightLp;
				HJ::Logger::Info(std::format("HJ GUARD: {} front {} motion={}", gate, wantLeft ? "L" : "R", motion));
			}
			return frontResult;
		}

		// +angle = left hit, 110 deg each way with front overlap
		if (wantLeft ? (angle < -kFrontOverlap || angle > kSideCone) : (angle > kFrontOverlap || angle < -kSideCone))
		{
			HJ::Logger::Info(std::format("HJ GUARD: {} deny side={} angle={:.3f}", gate, wantLeft ? "L" : "R", angle));
			return 0;
		}



		const std::uint8_t result = orig(owner, packet);
		if (result != 0)
		{
			// side anim swapped below, neutral stays on motion_set
			std::uint32_t motion = wantLeft ? kLeftLp : kRightLp;
			g_blockCount.fetch_add(1);
			HJ::Logger::Info(std::format("HJ GUARD: {} side {} angle={:.3f} motion={}", gate, wantLeft ? "L" : "R", angle, motion));
		}
		return result;
	}

	std::uint8_t __fastcall GuardGateHook(std::uintptr_t owner, std::uintptr_t packet)
	{
		return GuardGateImpl(owner, packet, CallGuardOrig1, "g1");
	}

	std::uint8_t __fastcall GuardGateHook2(std::uintptr_t owner, std::uintptr_t packet)
	{
		return GuardGateImpl(owner, packet, CallGuardOrig2, "g2");
	}

	std::uint8_t CallLiveDispatchOrig(std::uintptr_t owner, std::uintptr_t packet)
	{
		return g_liveDispatchHook.call<std::uint8_t>(owner, packet);
	}

	// deny on the proven live player path (0x142EF15A0)
	std::uint8_t __fastcall LiveDispatchHook(std::uintptr_t owner, std::uintptr_t packet)
	{
		return GuardGateImpl(owner, packet, CallLiveDispatchOrig, "gL");
	}

	// logs only, orig signature matched (owner, packet)
	std::uint8_t __fastcall GuardAllowedHook(std::uintptr_t owner, std::uintptr_t packet)
	{
		const std::uint8_t result = g_guardAllowedHook.call<std::uint8_t>(owner, packet);
		if (GetGuardSide() != GuardSide::None)
		{
			HJ::Logger::Info(std::format(
				"HJ GUARD: allowedcheck owner=0x{:X} caller=0x{:X} result={}",
				owner, reinterpret_cast<std::uintptr_t>(_ReturnAddress()), result));
		}
		return result;
	}

	// routing telemetry only: which hits reach the central dispatcher
	// while the player blocks, and do the gate offsets hold there.
	// never alters behavior, deny design follows from its lines.
	std::uint8_t __fastcall DispatchHook(std::uintptr_t owner, std::uintptr_t packet)
	{
		const GuardSide side = GetGuardSide();
		if (side != GuardSide::None)
		{
			const std::uintptr_t playerObject = HJ_GetPlayerCombatObject();
			float angle = 0.0f;
			const bool haveAngle = TryGetBlockAngle(owner, packet, angle);
			HJ::Logger::Info(std::format(
				"HJ GUARD: g3 owner=0x{:X} player=0x{:X} side={} hasAngle={} angle={:.3f}",
				owner, playerObject,
				side == GuardSide::Both ? "both" :
				side == GuardSide::Left ? "left" : "right",
				haveAngle, haveAngle ? angle : 0.0f));
		}
		return g_dispatchHook.call<std::uint8_t>(owner, packet);
	}
	bool HasGuardishName(const char* s) noexcept
	{
		for (int i = 0; i + 4 < 64 && s[i] != '\0'; ++i)
		{
			if ((s[i] == 'g' && s[i + 1] == 'r' && s[i + 2] == 'd') ||
				(s[i] == 'u' && s[i + 1] == 'a' && s[i + 2] == 'r' && s[i + 3] == 'd'))
				return true;
		}
		return false;
	}

	// motion-name resolver tap: if guard playback resolves names,
	// the names cross here while blocking, revealing the playback
	// path (and a future substitution point). logging only.
	std::uint64_t __fastcall ResolveHook(std::uintptr_t table, const char* name)
	{
		const std::uint64_t idx = g_resolveHook.call<std::uint64_t>(table, name);
		if (GetGuardSide() != GuardSide::None && name != nullptr)
		{
			char buf[64] = {};
			if (SafeReadBytes(reinterpret_cast<std::uintptr_t>(name), buf, sizeof(buf) - 1) &&
				HasGuardishName(buf))
			{
				HJ::Logger::Info(std::format(
					"HJ GUARD: resolve table=0x{:X} name={} idx={}",
					table, buf, idx & 0xFFFF));
			}
		}
		return idx;
	}
}

namespace HJ::Hooks::GuardDirection
{
	bool Initialize()
	{
		if (!kDirectionalBlockEnabled)
		{
			Logger::Info("Directional block disabled, guard hooks parked.");
			return true;
		}

		const HMODULE gameModule = GetModuleHandleA(nullptr);
		if (!gameModule)
		{
			Logger::Error("GuardDirection hook: no game module.");
			return false;
		}

		const auto base = reinterpret_cast<std::uintptr_t>(gameModule);

		if (!kHookGates)
		{
			Logger::Info("Guard gates parked for bisect.");
		}
		else
		{
		const auto target = base + kGuardGateRva;

		const std::uint8_t expected[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57 };
		if (std::memcmp(reinterpret_cast<void*>(target), expected, sizeof(expected)) != 0)
		{
			Logger::Error("GuardDirection hook: bytes dont match.");
			return false;
		}

		g_guardGateHook = safetyhook::create_inline(
			reinterpret_cast<void*>(target),
			reinterpret_cast<void*>(&GuardGateHook));

		if (!g_guardGateHook)
		{
			Logger::Error("GuardDirection hook: install failed.");
			return false;
		}

		Logger::Info("GuardDirection hook installed.");

		// second evaluator, same shape, same prologue, own trampoline
		const auto target2 = base + kGuardGate2Rva;

		if (std::memcmp(reinterpret_cast<void*>(target2), expected, sizeof(expected)) != 0)
		{
			Logger::Error("GuardDirection hook 2: bytes dont match.");
			return false;
		}

		g_guardGateHook2 = safetyhook::create_inline(
			reinterpret_cast<void*>(target2),
			reinterpret_cast<void*>(&GuardGateHook2));

		if (!g_guardGateHook2)
		{
			Logger::Error("GuardDirection hook 2: install failed.");
			return false;
		}

		Logger::Info("GuardDirection hook 2 installed.");
		}

		if (!kHookDispatch)
		{
			Logger::Info("Dispatch hook parked for bisect.");
		}
		else
		{
		// central reaction dispatcher, telemetry only (see DispatchHook)
		const auto dispatchTarget = base + kDispatchRva;

		const std::uint8_t expectedDispatch[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48 };
		if (std::memcmp(reinterpret_cast<void*>(dispatchTarget), expectedDispatch, sizeof(expectedDispatch)) != 0)
		{
			Logger::Error("Dispatch hook: bytes dont match.");
			return false;
		}

		g_dispatchHook = safetyhook::create_inline(
			reinterpret_cast<void*>(dispatchTarget),
			reinterpret_cast<void*>(&DispatchHook));

		if (!g_dispatchHook)
		{
			Logger::Error("Dispatch hook: install failed.");
			return false;
		}

		Logger::Info("Dispatch hook installed.");
		}

		if (!kHookLiveDispatch)
		{
			Logger::Info("Live dispatch hook parked.");
		}
		else
		{
		// proven live player guard path, deny enforced here
		const auto liveTarget = base + kLiveDispatchRva;

		const std::uint8_t expectedLive[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55 };
		if (std::memcmp(reinterpret_cast<void*>(liveTarget), expectedLive, sizeof(expectedLive)) != 0)
		{
			Logger::Error("Live dispatch hook: bytes dont match.");
			return false;
		}

		g_liveDispatchHook = safetyhook::create_inline(
			reinterpret_cast<void*>(liveTarget),
			reinterpret_cast<void*>(&LiveDispatchHook));

		if (!g_liveDispatchHook)
		{
			Logger::Error("Live dispatch hook: install failed.");
			return false;
		}

		Logger::Info("Live dispatch hook installed.");
		}

#if 1
		if (!kHookAllowed)
		{
			Logger::Info("GuardAllowed parked for bisect.");
		}
		else
		{
		const auto allowedTarget = base + kGuardAllowedRva;

		const std::uint8_t expectedAllowed[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
		if (std::memcmp(reinterpret_cast<void*>(allowedTarget), expectedAllowed, sizeof(expectedAllowed)) != 0)
		{
			Logger::Error("GuardAllowed hook: bytes dont match.");
			return false;
		}

		g_guardAllowedHook = safetyhook::create_inline(
			reinterpret_cast<void*>(allowedTarget),
			reinterpret_cast<void*>(&GuardAllowedHook));

		if (!g_guardAllowedHook)
		{
			Logger::Error("GuardAllowed hook: install failed.");
			return false;
		}

		Logger::Info("GuardAllowed hook installed.");
		}
#endif

#if 1
		if (!kHookResolve)
		{
			Logger::Info("Resolve hook parked for bisect.");
		}
		else
		{
		const auto resolveTarget = base + kResolveRva;

		const std::uint8_t expectedResolve[] = { 0x44, 0x0F, 0xB7, 0x49, 0x20, 0x45, 0x31, 0xC0 };
		if (std::memcmp(reinterpret_cast<void*>(resolveTarget), expectedResolve, sizeof(expectedResolve)) != 0)
		{
			Logger::Error("Resolve hook: bytes dont match.");
			return false;
		}

		g_resolveHook = safetyhook::create_inline(
			reinterpret_cast<void*>(resolveTarget),
			reinterpret_cast<void*>(&ResolveHook));

		if (!g_resolveHook)
		{
			Logger::Error("Resolve hook: install failed.");
			return false;
		}

		Logger::Info("Resolve hook installed.");
		}
#endif
		return true;
	}

	// per-frame swap so stance-enter reads the side triple, not just hit reactions
	void UpdateFromInput()
	{
		if (!FindGuardRow())
			return;

		const GuardSide side = GetGuardSide();
		const int triple =
			side == GuardSide::None ? -1 :
			side == GuardSide::Left ? 1 :
			side == GuardSide::Right ? 2 : 0;

		ApplyGuardTriple(triple);

		// engine refresh would silently eat the swap, catch it and rewrite
		if (triple >= 0)
		{
			static unsigned frame = 0;
			if ((++frame % 30) == 0)
			{
				const std::uint16_t wantLp =
					triple == 1 ? static_cast<std::uint16_t>(kLeftLp) :
					triple == 2 ? static_cast<std::uint16_t>(kRightLp) :
					g_neutralLp.load(std::memory_order_acquire);

				std::uint16_t haveLp = 0;
				const std::uintptr_t lpCell =
					g_guardLpCell.load(std::memory_order_acquire);

				if (!SafeReadU16(lpCell, haveLp) || haveLp != wantLp)
				{
					HJ::Logger::Info("HJ GUARD: row clobbered, rewriting");
					g_lastAppliedSide.store(-2, std::memory_order_release);
					ApplyGuardTriple(triple);
				}
			}
		}
	}

	// row is always vanilla outside combat
	void RestoreVanilla()
	{
		ApplyGuardTriple(-1);
	}

	int CurrentSideId()
	{
		const GuardSide side = GetGuardSide();
		return side == GuardSide::None ? -1 :
			side == GuardSide::Left ? 1 :
			side == GuardSide::Right ? 2 : 0;
	}

	std::atomic<bool> g_guardReentered{ false };

	void NotifyGuardEntered()
	{
		g_guardReentered.store(true, std::memory_order_release);
		g_holdSide.store(-2, std::memory_order_release);
		g_blockCount.store(0, std::memory_order_release);
	}

	bool TakeGuardReentered()
	{
		return g_guardReentered.exchange(false, std::memory_order_acq_rel);
	}

	// only a full triple adoption counts as live, lp-only mirrors
	// must never drive stance restarts on their own
	bool HasGuardCells()
	{
		return g_guardStCell.load(std::memory_order_acquire) != 0;
	}

	// the Guard stance references the motion row, find it by content:
	// every pointer in the state object and its container is followed
	// two levels deep and each target scored against known row values
	void ScanGuardObject(std::uintptr_t stateObject, std::uintptr_t stateContainer)
	{
		EnsureScanThread();

		if (FindGuardRow())
		{
			std::uint16_t curLp = 0;
			SafeReadU16(
				g_guardLpCell.load(std::memory_order_acquire),
				curLp);

			HJ::Logger::Info(std::format(
				"HJ GUARD: Guard entry obj=0x{:X} rowLp={} lastSide={}",
				stateObject, curLp,
				g_lastAppliedSide.load(std::memory_order_acquire)));
			return;
		}

		HJ::Logger::Info(std::format(
			"HJ GUARD: Guard entry obj=0x{:X} (scanning)",
			stateObject));

		constexpr int kNeed = 6;
		constexpr int kNeedles = sizeof(kRowNeedles) / sizeof(kRowNeedles[0]);
		constexpr int kWideU16 = 1024;
		constexpr int kBackU16 = 256;
		constexpr int kNarrowU16 = 96;
		std::uint16_t dump[kWideU16] = {};

		int bestHits = -1;
		std::uintptr_t bestAddr = 0;
		std::uintptr_t bestLpAddr = 0;
		bool bestLpVan = false;

		// score one window, adopt on triple win, else track best
		auto checkDump = [&](std::uintptr_t base, int count) -> bool
		{
			int hits = 0;
			int stAt = -1;
			int lpAt = -1;
			int enAt = -1;
			for (int w = 0; w < count; ++w)
			{
				for (int n = 0; n < kNeedles; ++n)
				{
					if (dump[w] == kRowNeedles[n])
					{
						++hits;
						break;
					}
				}
				if (dump[w] == kModSt)
					stAt = w;
				else if (dump[w] == kModLp || dump[w] == kLeftLp)
					lpAt = w;
				else if (dump[w] == kModEn)
					enAt = w;
			}

			if (hits >= kNeed && stAt >= 0 && lpAt >= 0 && enAt >= 0)
			{
				const std::uintptr_t stCell = base + static_cast<std::uintptr_t>(stAt) * 2;
				const std::uintptr_t lpCell = base + static_cast<std::uintptr_t>(lpAt) * 2;
				const std::uintptr_t enCell = base + static_cast<std::uintptr_t>(enAt) * 2;

				// neutrals come from the live cells, never constants,
				// so file edits can never desync restore
				std::uint16_t nSt = 0;
				std::uint16_t nLp = 0;
				std::uint16_t nEn = 0;
				if (SafeReadU16(stCell, nSt))
					g_neutralSt.store(nSt, std::memory_order_release);
				if (SafeReadU16(lpCell, nLp))
					g_neutralLp.store(nLp, std::memory_order_release);
				if (SafeReadU16(enCell, nEn))
					g_neutralEn.store(nEn, std::memory_order_release);

				g_guardStCell.store(stCell, std::memory_order_release);
				g_guardLpCell.store(lpCell, std::memory_order_release);
				g_guardEnCell.store(enCell, std::memory_order_release);

				HJ::Logger::Info(std::format(
					"HJ GUARD: row via Guard obj=0x{:X} st=0x{:X} lp=0x{:X} en=0x{:X} hits={}",
					stateObject,
					stCell, lpCell, enCell,
					hits));
				return true;
			}

			if (hits > bestHits)
			{
				bestHits = hits;
				bestAddr = base;
				bestLpAddr = 0;
				bestLpVan = false;
				for (int w = 0; w < count; ++w)
				{
					if (dump[w] == kLeftLp)
					{
						bestLpAddr = base + static_cast<std::uintptr_t>(w) * 2;
						break;
					}
				}
				if (bestLpAddr == 0)
				{
					for (int w = 0; w < count; ++w)
					{
						if (dump[w] == kModLp)
						{
							bestLpAddr = base + static_cast<std::uintptr_t>(w) * 2;
							break;
						}
					}
				}
				if (bestLpAddr == 0)
				{
					for (int w = 0; w < count; ++w)
					{
						if (dump[w] == kVanLp)
						{
							bestLpAddr = base + static_cast<std::uintptr_t>(w) * 2;
							bestLpVan = true;
							break;
						}
					}
				}
			}
			return false;
		};

		// one target: wide window forward, shorter window behind it
		auto tryAdopt = [&](std::uintptr_t target, bool wide) -> bool
		{
			const int fwd = wide ? kWideU16 : kNarrowU16;
			if (ReadU16Block(target, dump, fwd) && checkDump(target, fwd))
				return true;
			if (wide && ReadU16Block(target - 512, dump, kBackU16) &&
				checkDump(target - 512, kBackU16))
				return true;
			return false;
		};

		const std::uintptr_t roots[2] = { stateObject, stateContainer };
		for (int r = 0; r < 2 && !FindGuardRow(); ++r)
		{
			if (roots[r] == 0)
				continue;

			std::uintptr_t l1[128];
			int nl1 = 0;

			for (int i = 0; i < 128 && !FindGuardRow(); ++i)
			{
				std::uintptr_t target = 0;
				if (!SafeReadPtr(roots[r] + static_cast<std::uintptr_t>(i) * 8, target) ||
					target == 0)
					continue;

				if (nl1 < 128)
					l1[nl1++] = target;

				if (tryAdopt(target, true))
					return;
			}

			// second hop for object -> style -> row nesting
			const int nl2 = nl1 < 64 ? nl1 : 64;
			for (int i = 0; i < nl2 && !FindGuardRow(); ++i)
			{
				for (int j = 0; j < 32 && !FindGuardRow(); ++j)
				{
					std::uintptr_t target = 0;
					if (!SafeReadPtr(l1[i] + static_cast<std::uintptr_t>(j) * 8, target) ||
						target == 0)
						continue;

					if (tryAdopt(target, false))
						return;
				}
			}
		}

		// lp-only fallback: the hold-loop cell is verified live even
		// when st/en sit elsewhere, the loop is what you stare at.
		// fresh test values win over stale ones, neutrals are read
		// back so restore always mirrors the file
		if (bestLpAddr != 0 && bestHits >= 3)
		{
			std::uint16_t curLp = 0;
			if (SafeReadU16(bestLpAddr, curLp) &&
				(curLp == kLeftLp || curLp == kModLp || curLp == kVanLp))
			{
				g_neutralLp.store(curLp, std::memory_order_release);
				g_guardLpCell.store(bestLpAddr, std::memory_order_release);

				HJ::Logger::Info(std::format(
					"HJ GUARD: row via Guard lp-only obj=0x{:X} lp=0x{:X} val={} hits={}",
					stateObject, bestLpAddr, curLp, bestHits));
				return;
			}
		}

		HJ::Logger::Info(std::format(
			"HJ GUARD: no row in Guard roots obj=0x{:X} best={} @0x{:X}",
			stateObject, bestHits, bestAddr));

		if (bestHits >= 0)
		{
			std::uint16_t best[256] = {};
			if (ReadU16Block(bestAddr, best, 256))
			{
				std::string vals;
				int fSt = -1;
				int fLp = -1;
				int fEn = -1;
				for (int i = 0; i < 256; ++i)
				{
					if (best[i] == kModSt)
						fSt = i;
					if (best[i] == kModLp)
						fLp = i;
					if (best[i] == kModEn)
						fEn = i;
					vals += std::format("{} ", best[i]);
				}
				HJ::Logger::Info(std::format(
					"HJ GUARD: best window @0x{:X} hits={} st@{} lp@{} en@{}: {}",
					bestAddr, bestHits, fSt, fLp, fEn, vals));
			}
		}
	}

	// POD-only copy, __try is legal here
	static bool CopyReactionBytes(std::uintptr_t object, std::uint8_t* out) noexcept
	{
		__try
		{
			std::memcpy(out, reinterpret_cast<const void*>(object), 0x3E0);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static bool CopyCString(std::uintptr_t target, char* out) noexcept
	{
		__try
		{
			std::memcpy(out, reinterpret_cast<const void*>(target), 63);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static std::uintptr_t ResolveHandle(std::uint32_t handle, short wantType) noexcept
	{
		const std::uintptr_t base =
			reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (base == 0 || (handle & 0xFFFFF) >= 0x80000)
			return 0;
		__try
		{
			const std::uintptr_t slot = (handle & 0xFFFFF) * 0x20;
			if (*reinterpret_cast<const std::uint16_t*>(base + 0x430EDC8 + slot) != (handle >> 0x14))
				return 0;
			if (*reinterpret_cast<const short*>(base + 0x430EDCC + slot) != wantType)
				return 0;
			return *reinterpret_cast<std::uintptr_t*>(base + 0x430EDC0 + slot);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	static bool CopyBytes(std::uintptr_t target, std::uint8_t* out, std::size_t n) noexcept
	{
		__try
		{
			std::memcpy(out, reinterpret_cast<const void*>(target), n);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void DumpGuardReaction(std::uintptr_t object, int side)
	{
		static std::uint8_t buf[0x3E0];
		static std::atomic<int> s_count[3]{};
		if (!CopyReactionBytes(object, buf))
		{
			HJ::Logger::Info("HJ GUARD: reaction dump unreadable");
			return;
		}
		const int n = side >= 0 && side < 3 ? s_count[side].fetch_add(1) : 99;
		for (int line = 0; line < 8; ++line)
		{
			std::string hex;
			hex.reserve(256);
			for (int i = 0; i < 124 && line * 124 + i < 0x3E0; ++i)
				hex += std::format("{:02X}", buf[line * 124 + i]);
			HJ::Logger::Info(std::format(
				"HJ GUARD: react side={} line={} {}",
				side, line, hex));
		}
		if (n >= 3)
			return;
		std::uint32_t entHandle = 0;
		std::memcpy(&entHandle, buf + 0x3D0, 4);
		const std::uintptr_t entity = ResolveHandle(entHandle, 3);
		std::uint32_t motHandle = 0;
		std::uintptr_t motion = 0;
		if (entity != 0 && CopyBytes(entity + 0xC4, reinterpret_cast<std::uint8_t*>(&motHandle), 4))
			motion = ResolveHandle(motHandle, 3);
		if (motion == 0 && entity != 0 && motHandle != 0)
			motion = ResolveHandle(motHandle, 4);
		HJ::Logger::Info(std::format(
			"HJ GUARD: reactent side={} entH={} entity=0x{:X} motH={} motion=0x{:X}",
			side, entHandle, entity, motHandle, motion));
		if (motion == 0)
			return;
		static std::uint8_t mbuf[0x400];
		if (!CopyBytes(motion, mbuf, sizeof(mbuf)))
		{
			HJ::Logger::Info("HJ GUARD: motion unreadable");
			return;
		}
		static const std::uint16_t kWant[] = { 13919, 15186, 5559, 300, 15187, 15180, 16317, 16321 };
		std::string hits;
		for (int off = 0; off + 2 <= 0x400; off += 2)
		{
			std::uint16_t v = 0;
			std::memcpy(&v, mbuf + off, 2);
			for (std::uint16_t w : kWant)
			{
				if (v == w)
					hits += std::format("{}:{} ", off, v);
			}
		}
		HJ::Logger::Info(std::format(
			"HJ GUARD: motscan side={} {}",
			side, hits.empty() ? "(none)" : hits));
		for (int off = 0; off + 8 <= 0x3E0; off += 8)
		{
			std::uintptr_t target = 0;
			std::memcpy(&target, buf + off, 8);
			if (target < 0x10000)
				continue;
			char str[64]{};
			if (!CopyCString(target, str))
				continue;
			int len = 0;
			while (len < 63 && str[len] != '\0')
			{
				if (str[len] < 32 || str[len] > 126)
					break;
				++len;
			}
			if (len >= 5 && str[len] == '\0')
			{
				HJ::Logger::Info(std::format(
					"HJ GUARD: reactstr side={} off={} {}",
					side, off, std::string_view(str, len)));
			}
		}
	}
	using SlotResolveFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
	static bool SlotToEntity(SlotResolveFn fn, std::uintptr_t owner, std::uintptr_t& slot, std::uintptr_t& entity) noexcept
	{
		__try
		{
			slot = fn(owner + 0x30);
			entity = slot != 0 ? *reinterpret_cast<std::uintptr_t*>(slot) : 0;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	void ScanStanceMotion(std::uintptr_t owner, int side)
	{
		static std::atomic<int> s_count[3]{};
		if (side < 0 || side > 2 || s_count[side].fetch_add(1) >= 3)
			return;
		const auto base =
			reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (base == 0)
			return;
		using SlotResolveFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
		auto slotResolve = reinterpret_cast<SlotResolveFn>(base + kSlotResolveRva);
		if (!slotResolve)
			return;
		std::uintptr_t slot = 0;
		std::uintptr_t entity = 0;
		SlotToEntity(slotResolve, owner, slot, entity);
		std::uint32_t motHandle = 0;
		std::uintptr_t motion = 0;
		if (entity != 0 && CopyBytes(entity + 0xC4, reinterpret_cast<std::uint8_t*>(&motHandle), 4))
			motion = ResolveHandle(motHandle, 3);
		if (motion == 0 && entity != 0 && motHandle != 0)
			motion = ResolveHandle(motHandle, 4);
		HJ::Logger::Info(std::format(
			"HJ GUARD: stancemotion side={} slot=0x{:X} entity=0x{:X} motH={} motion=0x{:X}",
			side, slot, entity, motHandle, motion));
		static std::uint8_t ebuf[0x600];
		if (entity != 0 && CopyBytes(entity, ebuf, sizeof(ebuf)))
		{
			static const std::uint16_t kWantE[] = { 13919, 15186, 5559, 300, 15187, 15180, 16317, 16321 };
			std::string ehits;
			for (int off = 0; off + 2 <= 0x600; off += 2)
			{
				std::uint16_t v = 0;
				std::memcpy(&v, ebuf + off, 2);
				for (std::uint16_t w : kWantE)
				{
					if (v == w)
						ehits += std::format("{}:{} ", off, v);
				}
			}
			HJ::Logger::Info(std::format(
				"HJ GUARD: entityscan side={} {}",
				side, ehits.empty() ? "(none)" : ehits));
		}
		if (motion == 0)
			return;
		static std::uint8_t mbuf[0x400];
		if (!CopyBytes(motion, mbuf, sizeof(mbuf)))
		{
			HJ::Logger::Info("HJ GUARD: stance motion unreadable");
			return;
		}
		static const std::uint16_t kWant[] = { 13919, 15186, 5559, 300, 15187, 15180, 16317, 16321 };
		std::string hits;
		for (int off = 0; off + 2 <= 0x400; off += 2)
		{
			std::uint16_t v = 0;
			std::memcpy(&v, mbuf + off, 2);
			for (std::uint16_t w : kWant)
			{
				if (v == w)
					hits += std::format("{}:{} ", off, v);
			}
		}
		HJ::Logger::Info(std::format(
			"HJ GUARD: stancescan side={} {}",
			side, hits.empty() ? "(none)" : hits));
	}
}
