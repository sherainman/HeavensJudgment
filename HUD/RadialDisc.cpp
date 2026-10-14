// vinyl disc driver: finds the record quadrants in the live uiManager
// table and pushes hp into them. read-only until the dump checks out,
// then native writes only. scene 301, layout layer row 1190.

#include "RadialDisc.hpp"

#include "RadialHealthBridge.hpp"

#include "../Core/Logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	std::uintptr_t g_moduleBase = 0;

	constexpr std::uintptr_t kUiManagerRva = 0x041F0D80;

	constexpr std::uintptr_t kAnchorHandleOffset = 0x180;
	constexpr std::uintptr_t kAnchorHandleOffset2 = 0x178;

	constexpr std::uint32_t kLayoutLayerRow = 1190;

	constexpr std::uint32_t kSceneLifeGauge = 0x12D; // 301

	constexpr float kQuadrantX = -530.0f;
	constexpr float kQuadrantY = 23.077f;
	constexpr float kQuadrantSize = 36.0f;
	constexpr float kQuadrantIvoryR = 240.0f / 255.0f;
	constexpr float kQuadrantIvoryG = 239.0f / 255.0f;
	constexpr float kQuadrantIvoryB = 229.0f / 255.0f;
	constexpr float kQuadrantIvoryA = 242.0f / 255.0f;

	constexpr std::size_t kObjectDumpBytes = 0x200;
	constexpr std::size_t kCellScanBytes = 0x400;
	constexpr std::size_t kCellDumpQwords = 0x80; 

	constexpr std::uint64_t kTagSceneRoot =
		(static_cast<std::uint64_t>(1) << 32) | kSceneLifeGauge;
	constexpr std::uint64_t kTagSceneLayout =
		(static_cast<std::uint64_t>(7) << 32) | kSceneLifeGauge;

	constexpr std::size_t kElemSlotCount = 11;
	constexpr std::size_t kElemSlotStride = 0x90; // table-247 stride guess

	std::atomic<bool> g_dumpDone{ false };
	std::atomic<std::uintptr_t> g_lastDumpedAnchor{ 0 };
	std::atomic<float> g_lastDumpedRatio{ 2.0f }; // force first dump
	std::atomic<int> g_dumpCount{ 0 };
	constexpr int kMaxDumps = 4;

	std::uint64_t ReadU64(std::uintptr_t address)
	{
		return *reinterpret_cast<std::uint64_t*>(address);
	}

	std::uintptr_t ResolveObject(std::uint64_t handle)
	{
		if (!handle || !g_moduleBase)
			return 0;

		const auto uiManager =
			*reinterpret_cast<std::uintptr_t*>(
				g_moduleBase + kUiManagerRva);

		if (!uiManager)
			return 0;

		const auto capacity =
			*reinterpret_cast<std::uint32_t*>(
				uiManager + 0x19C);

		const auto low = static_cast<std::uint32_t>(handle);

		if (!low)
			return 0;

		const std::uint32_t index = low - 1;

		if (index >= capacity)
			return 0;

		const auto objects =
			*reinterpret_cast<std::uintptr_t*>(
				uiManager + 0x190);

		if (!objects)
			return 0;

		const auto object =
			*reinterpret_cast<std::uintptr_t*>(
				objects +
				static_cast<std::uintptr_t>(index) *
				sizeof(std::uintptr_t));

		if (!object)
			return 0;

		if (ReadU64(object + 0x70) != handle)
			return 0;

		return object;
	}

	std::string H8(std::uint64_t value)
	{
		std::ostringstream os;
		os << "0x" << std::uppercase << std::hex
		   << std::setw(16) << std::setfill('0') << value;
		return os.str();
	}

	std::size_t ReadableSpan(
		std::uintptr_t address,
		std::size_t want)
	{
		if (!address || address < 0x10000)
			return 0;

		MEMORY_BASIC_INFORMATION mbi = {};

		if (VirtualQuery(
				reinterpret_cast<LPCVOID>(address),
				&mbi,
				sizeof(mbi)) == 0)
		{
			return 0;
		}

		const auto base =
			reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);

		if (mbi.State != MEM_COMMIT ||
			(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
							PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
							PAGE_EXECUTE_WRITECOPY | PAGE_WRITECOPY)) == 0)
		{
			return 0;
		}

		const std::uintptr_t regionEnd = base + mbi.RegionSize;

		if (address >= regionEnd)
			return 0;

		const std::size_t avail =
			static_cast<std::size_t>(regionEnd - address);

		return avail < want ? avail : want;
	}

	bool ScanFloatSignature(
		std::uintptr_t address,
		std::size_t bytes,
		float& x, float& y, float& size,
		int& hitOffset)
	{
		x = y = size = 0.0f;
		hitOffset = -1;

		bool xHit = false;
		bool yHit = false;
		bool sizeHit = false;

		if (!address || bytes < sizeof(float))
			return false;

		const auto raw =
			reinterpret_cast<const std::uint8_t*>(address);

		for (std::size_t off = 0;
			off + sizeof(float) <= bytes;
			off += sizeof(float))
		{
			float v = 0.0f;

			std::memcpy(&v, raw + off, sizeof(v));

			if (!std::isfinite(v))
				continue;

			if (!xHit && std::fabs(v - kQuadrantX) < 2.0f)
			{
				x = v;
				xHit = true;
				hitOffset = static_cast<int>(off);
			}

			if (!yHit && std::fabs(v - kQuadrantY) < 0.5f)
			{
				y = v;
				yHit = true;
			}

			if (!sizeHit && std::fabs(v - kQuadrantSize) < 1.0f)
			{
				size = v;
				sizeHit = true;
			}
		}

		return xHit && sizeHit;
	}


	struct TableObj
	{
		std::uint32_t index = 0;
		std::uint64_t handle = 0;
		std::uintptr_t object = 0;
		std::uintptr_t vtable = 0;
		std::uintptr_t parent = 0;
		std::uint64_t tag = 0;       
		std::uintptr_t cell = 0;     

		int parentIndex = -1;       
		bool sceneMember = false;    
		bool cellXSize = false;      
		int cellXHitOffset = -1;
		float cellX = 0.0f;
		float cellY = 0.0f;
		float cellSize = 0.0f;
	};

	std::vector<TableObj> CollectTable(
		std::uintptr_t uiManager,
		std::uint32_t capacity,
		std::uintptr_t objects)
	{
		std::vector<TableObj> result;

		for (std::uint32_t index = 0; index < capacity; ++index)
		{
			const auto object =
				*reinterpret_cast<std::uintptr_t*>(
					objects +
					static_cast<std::uintptr_t>(index) *
					sizeof(std::uintptr_t));

			if (!object)
				continue;

			const auto handle = ReadU64(object + 0x70);

			if (!handle)
				continue;

			
			if (static_cast<std::uint32_t>(handle) != index + 1)
				continue;

			TableObj entry;
			entry.index = index;
			entry.handle = handle;
			entry.object = object;
			entry.vtable = ReadU64(object);
			entry.parent = ReadU64(object + 0x68);
			entry.tag = ReadU64(object + 0xB8);
			entry.cell =
				static_cast<std::uintptr_t>(ReadU64(object + 0xC8));
			entry.sceneMember =
				(static_cast<std::uint32_t>(entry.tag) & 0xFFFF) ==
				kSceneLifeGauge;

			
			if (entry.cell >= 0x10000 &&
				entry.cell < 0x0000800000000000ULL)
			{
				float x = 0.0f;
				float y = 0.0f;
				float size = 0.0f;
				int hitOff = -1;

				if (ScanFloatSignature(
						entry.cell, kCellScanBytes,
						x, y, size, hitOff))
				{
					entry.cellXSize = true;
					entry.cellXHitOffset = hitOff;
					entry.cellX = x;
					entry.cellY = y;
					entry.cellSize = size;
				}
			}

			result.push_back(entry);
		}

		
		for (auto& entry : result)
		{
			if (!entry.parent)
				continue;

			for (std::size_t i = 0; i < result.size(); ++i)
			{
				if (result[i].object == entry.parent)
				{
					entry.parentIndex = static_cast<int>(i);
					break;
				}
			}
		}

		return result;
	}

	std::string IndexHex(std::uint32_t value)
	{
		std::ostringstream os;
		os << "0x" << std::uppercase << std::hex
		   << std::setw(4) << std::setfill('0') << value;
		return os.str();
	}

	void DumpQwords(
		std::ofstream& out,
		std::uintptr_t address,
		std::size_t qwordCount,
		const char* indent,
		bool annotateFloats)
	{
		if (!address)
			return;

		const auto bytes =
			reinterpret_cast<const std::uint8_t*>(address);

		for (std::size_t q = 0; q < qwordCount; ++q)
		{
			const auto off = q * sizeof(std::uint64_t);

			std::uint64_t v = 0;

			std::memcpy(&v, bytes + off, sizeof(v));

			out
				<< indent << "+0x"
				<< std::hex << std::uppercase << std::setw(3)
				<< std::setfill('0') << off << std::dec
				<< "  " << H8(v);

			if (annotateFloats)
			{
				float f = 0.0f;

				std::memcpy(&f, &v, sizeof(f));

				if (std::isfinite(f) &&
					(std::fabs(f - kQuadrantX) < 2.0f ||
					 std::fabs(f - kQuadrantY) < 0.5f ||
					 std::fabs(f - kQuadrantSize) < 1.0f ||
					 std::fabs(f - kQuadrantIvoryR) < 0.01f ||
					 std::fabs(f - kQuadrantIvoryG) < 0.01f ||
					 std::fabs(f - kQuadrantIvoryB) < 0.01f ||
					 std::fabs(f - kQuadrantIvoryA) < 0.01f))
				{
					out << "  <<< float " << f;
				}
			}

			out << '\n';
		}
	}

	void DumpControllerHandles(
		std::ofstream& out,
		std::uintptr_t controller)
	{
		out << "controller inline handle block (+0x160..+0x1F8):\n";

		for (std::size_t off = 0x160;
			off <= 0x1F8;
			off += sizeof(std::uint64_t))
		{
			const auto handle = ReadU64(controller + off);

			const auto object = ResolveObject(handle);

			out
				<< "  +0x"
				<< std::hex << std::uppercase << std::setw(3)
				<< std::setfill('0') << off << std::dec
				<< "  handle=" << H8(handle);

			if (object)
			{
				out
					<< "  -> object=" << H8(object)
					<< " vtable=" << H8(ReadU64(object));
			}
			else
			{
				out << "  (no object)";
			}

			out << '\n';
		}
	}

	void EmitCompactList(
		std::ofstream& out,
		const std::vector<TableObj>& table)
	{
		out
			<< "\n== COMPACT VALID OBJECT LIST (whole table) ==\n"
			<< "  idx | vtable | parent | tag | cell\n";

		for (const auto& e : table)
		{
			out << "  " << IndexHex(e.index)
				<< " vt=" << H8(e.vtable)
				<< " parent=";

			if (e.parentIndex >= 0)
				out << IndexHex(table[e.parentIndex].index);
			else
				out << "-";

			out << " tag=" << H8(e.tag)
				<< " cell=";

			if (e.cell)
				out << H8(e.cell);
			else
				out << "-";

			if (e.sceneMember)
				out << " <scene301>";

			if (e.cellXSize)
				out << " <<< CELL X&SIZE";

			out << '\n';
		}

		out << "  total valid objects: " << table.size() << '\n';
	}

	void EmitSceneTree(
		std::ofstream& out,
		const std::vector<TableObj>& table)
	{
		out
			<< "\n== SCENE 301 TREE (tag low16 = 0x"
			<< std::hex << std::uppercase << kSceneLifeGauge << std::dec
			<< ") ==\n";

		int members = 0;

		for (const auto& e : table)
		{
			if (!e.sceneMember)
				continue;

			members++;

			out << "  " << IndexHex(e.index)
				<< " vt=" << H8(e.vtable)
				<< " parentIdx=";

			if (e.parentIndex >= 0)
				out << IndexHex(table[e.parentIndex].index);
			else
				out << "-";

			out << " tag=" << H8(e.tag)
				<< " cell=";

			if (e.cell)
				out << H8(e.cell);
			else
				out << "-";

			if (e.cellXSize)
				out << "  <<< CELL X&SIZE";

			out << '\n';
		}

		out << "  total scene-301 members: " << members << '\n';
	}

	void EmitCellHits(
		std::ofstream& out,
		const std::vector<TableObj>& table)
	{
		out
			<< "\n== CELL SIGNATURE HITS (x=-530 & size=36 "
			<< "inside +0xC8 pointee) ==\n";

		int hits = 0;

		for (const auto& e : table)
		{
			if (!e.cellXSize)
				continue;

			hits++;

			out
				<< "  " << IndexHex(e.index)
				<< " cell=" << H8(e.cell)
				<< " hitOff=0x"
				<< std::hex << std::uppercase << std::setw(3)
				<< std::setfill('0') << e.cellXHitOffset << std::dec
				<< " x=" << e.cellX
				<< " y=" << e.cellY
				<< " size=" << e.cellSize
				<< (e.sceneMember ? " <scene301>" : "")
				<< '\n';
		}

		out << "  cell signature hits: " << hits << '\n';
	}

	void ScanTargetForWedge(
		std::ofstream& out,
		const char* label,
		std::uintptr_t ptr,
		std::size_t bytes)
	{
		if (!ptr || ptr < 0x10000 ||
			ptr > 0x0000800000000000ULL)
		{
			out << "  [target " << label << "] invalid ptr\n";
			return;
		}

		const std::size_t safeBytes = ReadableSpan(ptr, bytes);

		if (safeBytes < sizeof(float))
		{
			out << "  [target " << label << "] ptr=" << H8(ptr)
				<< " not readable\n";
			return;
		}

		const auto raw =
			reinterpret_cast<const std::uint8_t*>(ptr);

		struct Hit
		{
			std::size_t off = 0;
			std::string kind;
		};

		std::vector<Hit> hits;

		for (std::size_t off = 0;
			off + sizeof(float) <= safeBytes;
			off += sizeof(float))
		{
			float f = 0.0f;
			std::uint32_t u = 0;

			std::memcpy(&f, raw + off, sizeof(f));
			std::memcpy(&u, raw + off, sizeof(u));

			if (std::isfinite(f) && std::fabs(f - kQuadrantX) < 2.0f)
				hits.push_back({ off, "X=-530" });

			if (std::isfinite(f) && std::fabs(f - kQuadrantY) < 0.5f)
				hits.push_back({ off, "Y=23.077" });

			if (std::isfinite(f) && std::fabs(f - kQuadrantSize) < 1.0f)
				hits.push_back({ off, "SIZE=36" });

			if (std::isfinite(f) && std::fabs(f - kQuadrantIvoryR) < 0.01f)
				hits.push_back({ off, "IVORY_R" });

			if (std::isfinite(f) && std::fabs(f - kQuadrantIvoryG) < 0.01f)
				hits.push_back({ off, "IVORY_G" });

			if (std::isfinite(f) && std::fabs(f - kQuadrantIvoryB) < 0.01f)
				hits.push_back({ off, "IVORY_B" });

			if (std::isfinite(f) && std::fabs(f - kQuadrantIvoryA) < 0.01f)
				hits.push_back({ off, "IVORY_A" });

			if (u == 73)
				hits.push_back({ off, "SPRITE_73" });

	
			if (u == 9)
				hits.push_back({ off, "PIECE_ID" });
		}

		out << "  [target " << label << "] ptr=" << H8(ptr)
			<< " scanned=" << std::hex << safeBytes << std::dec
			<< " hits=" << hits.size() << '\n';


		std::sort(hits.begin(), hits.end(),
			[](const Hit& a, const Hit& b)
			{
				if (a.off / kElemSlotStride != b.off / kElemSlotStride)
					return a.off / kElemSlotStride < b.off / kElemSlotStride;
				return a.off < b.off;
			});

		for (const auto& h : hits)
		{
			out << "    +0x" << std::hex << std::uppercase << std::setw(4)
				<< std::setfill('0') << h.off << std::dec
				<< " (slot " << (h.off / kElemSlotStride) << ", offset "
				<< (h.off % kElemSlotStride) << ")  " << h.kind << '\n';
		}
	}


	void CollectHeapPointers(
		std::uintptr_t ptr,
		std::size_t bytes,
		std::vector<std::uintptr_t>& out)
	{
		if (!ptr || ptr < 0x10000 ||
			ptr > 0x0000800000000000ULL)
			return;

		const std::size_t safe = ReadableSpan(ptr, bytes);

		if (safe < sizeof(std::uint64_t))
			return;

		const auto raw =
			reinterpret_cast<const std::uint8_t*>(ptr);

		for (std::size_t off = 0;
			off + sizeof(std::uint64_t) <= safe;
			off += sizeof(std::uint64_t))
		{
			std::uint64_t v = 0;

			std::memcpy(&v, raw + off, sizeof(v));

		
			if (v >= 0x7FF4C0000000ULL && v <= 0x7FF500000000ULL)
				out.push_back(static_cast<std::uintptr_t>(v));
		}
	}


	void EmitElementTargetScan(
		std::ofstream& out,
		const std::vector<TableObj>& table)
	{
		out
			<< "\n== ELEMENT POINTER-BLOCK TARGET SCAN (build 1c) ==\n"
			<< "  follows cell+0x18 / +0x218 / +0x220 / +0x230 "
			<< "(layout) and root +0x1F8 / +0x200 / +0x210 "
			<< "and scans each allocation for the wedge signature.\n";

		const TableObj* rootEntry = nullptr;
		const TableObj* layoutEntry = nullptr;

		for (const auto& e : table)
		{
			if (e.tag == kTagSceneRoot)
				rootEntry = &e;

			if (e.tag == kTagSceneLayout)
				layoutEntry = &e;
		}

		if (!rootEntry || !layoutEntry)
		{
			out << "  root/layout not found in table\n";
			return;
		}

		out << "  root obj=" << H8(rootEntry->object)
			<< " cell=" << H8(rootEntry->cell) << '\n';
		out << "  layout obj=" << H8(layoutEntry->object)
			<< " cell=" << H8(layoutEntry->cell) << '\n';

		const std::uintptr_t rootCell = rootEntry->cell;
		const std::uintptr_t layoutCell = layoutEntry->cell;

		auto dumpBlock = [&](const char* name, std::uintptr_t cell,
			std::size_t startOff, std::size_t endOff)
		{
			out << "  " << name << " ptr block [+0x"
				<< std::hex << startOff << " .. +0x" << endOff
				<< "]:\n" << std::dec;

			if (!cell || cell < 0x10000 ||
				cell > 0x0000800000000000ULL)
			{
				out << "    (no cell)\n";
				return;
			}

			for (std::size_t off = startOff;
				off <= endOff;
				off += sizeof(std::uint64_t))
			{
				const std::uint64_t v = ReadU64(cell + off);

				out << "    +0x" << std::hex << std::uppercase
					<< std::setw(3) << std::setfill('0') << off << std::dec
					<< "  " << H8(v);

				if (v >= 0x10000 && v < 0x0000800000000000ULL)
				{
					out << "  <ptr>";
				}

				out << '\n';
			}
		};

		dumpBlock("layout", layoutCell, 0x18, 0x18);
		dumpBlock("layout", layoutCell, 0x210, 0x240);
		dumpBlock("root", rootCell, 0x1F0, 0x220);


		struct Target
		{
			const char* label = nullptr;
			std::uintptr_t ptr = 0;
		};

		std::vector<Target> targets;

		auto addTarget = [&](const char* label, std::uintptr_t cell,
			std::size_t off)
		{
			if (!cell)
				return;

			const std::uint64_t v = ReadU64(cell + off);

			if (v >= 0x10000 && v < 0x0000800000000000ULL)
				targets.push_back({ label, static_cast<std::uintptr_t>(v) });
		};

		addTarget("layout.cell+0x18", layoutCell, 0x18);
		addTarget("layout.cell+0x218", layoutCell, 0x218);
		addTarget("layout.cell+0x220", layoutCell, 0x220);
		addTarget("layout.cell+0x230", layoutCell, 0x230);
		addTarget("root.cell+0x1F8", rootCell, 0x1F8);
		addTarget("root.cell+0x200", rootCell, 0x200);
		addTarget("root.cell+0x210", rootCell, 0x210);

		constexpr std::size_t kTargetScanBytes =
			11 * kElemSlotStride + 0x40; 

		for (const auto& t : targets)
			ScanTargetForWedge(out, t.label, t.ptr, kTargetScanBytes);

		out << "\n  == target dumps ==\n";

		for (const auto& t : targets)
		{
			const std::size_t safeBytes =
				ReadableSpan(t.ptr, kTargetScanBytes);

			out << "  --- target " << t.label << " ptr="
				<< H8(t.ptr) << " safe="
				<< std::hex << safeBytes << std::dec << " ---\n";

			DumpQwords(out, t.ptr,
				safeBytes / sizeof(std::uint64_t), "    ", true);
		}


		out << "\n== BUILD 1d CHILD FAN-OUT SCAN ==\n";

		std::vector<std::vector<std::uintptr_t>> perTarget;

		for (const auto& t : targets)
		{
			std::vector<std::uintptr_t> lst;

			CollectHeapPointers(t.ptr, kTargetScanBytes, lst);

			
			lst.erase(
				std::remove(lst.begin(), lst.end(), t.ptr),
				lst.end());

			std::sort(lst.begin(), lst.end());
			lst.erase(std::unique(lst.begin(), lst.end()), lst.end());

			perTarget.push_back(lst);
		}

		std::vector<std::uintptr_t> children;

		for (const auto& lst : perTarget)
		{
			children.insert(
				children.end(), lst.begin(), lst.end());
		}

		std::sort(children.begin(), children.end());
		children.erase(
			std::unique(children.begin(), children.end()),
			children.end());

		out << "  children per target:\n";

		for (std::size_t i = 0; i < targets.size(); ++i)
		{
			out << "    " << targets[i].label << ": "
				<< perTarget[i].size() << " children\n";

			for (const auto& c : perTarget[i])
				out << "        " << H8(c) << '\n';
		}

		out << "  unique child pointers: " << children.size() << '\n';

		for (const auto& c : children)
			ScanTargetForWedge(out, "child", c, kTargetScanBytes);

		if (!children.empty())
		{
			out << "\n  == build 1d child dumps ==\n";

			for (const auto& c : children)
			{
				const std::size_t safeBytes =
					ReadableSpan(c, kTargetScanBytes);

				out << "  --- child ptr=" << H8(c)
					<< " safe=" << std::hex << safeBytes << std::dec
					<< " ---\n";

				DumpQwords(out, c,
					safeBytes / sizeof(std::uint64_t), "    ", true);
			}
		}
	}

	void EmitFullDumps(
		std::ofstream& out,
		const std::vector<TableObj>& table,
		const std::vector<std::uint32_t>& wanted)
	{
		out << "\n== FULL QWORD DUMPS (scene closure + cell hits) ==\n";

		for (std::uint32_t idx : wanted)
		{
			const TableObj* entry = nullptr;

			for (const auto& e : table)
			{
				if (e.index == idx)
				{
					entry = &e;
					break;
				}
			}

			if (!entry)
				continue;

			out
				<< "  index=" << IndexHex(idx)
				<< " handle=" << H8(entry->handle)
				<< " object=" << H8(entry->object)
				<< " vtable=" << H8(entry->vtable)
				<< " tag=" << H8(entry->tag)
				<< " parentAtU68=" << H8(entry->parent)
				<< " sceneMember=" << (entry->sceneMember ? 1 : 0)
				<< " cellXSize=" << (entry->cellXSize ? 1 : 0)
				<< '\n';

			DumpQwords(
				out,
				entry->object,
				kObjectDumpBytes / sizeof(std::uint64_t),
				"    ",
				true);

			if (entry->cell)
			{
				out << "    [cell +0xC8 pointee]\n";

				DumpQwords(
					out,
					entry->cell,
					kCellDumpQwords,
					"      ",
					true);
			}
		}
	}

	void RunDump(std::uintptr_t controller, float health)
	{
		if (!g_moduleBase)
			return;

		const auto uiManager =
			*reinterpret_cast<std::uintptr_t*>(
				g_moduleBase + kUiManagerRva);

		if (!uiManager)
		{
			HJ::Logger::Warning("[RADIAL DISC] dump: no uiManager");
			return;
		}

		const auto capacity =
			*reinterpret_cast<std::uint32_t*>(
				uiManager + 0x19C);

		const auto objects =
			*reinterpret_cast<std::uintptr_t*>(
				uiManager + 0x190);

		if (!objects || !capacity)
		{
			HJ::Logger::Warning("[RADIAL DISC] dump: no object table");
			return;
		}

		auto table = CollectTable(uiManager, capacity, objects);

		const std::uint64_t anchor =
			ReadU64(controller + kAnchorHandleOffset);

		const std::uint64_t anchor2 =
			ReadU64(controller + kAnchorHandleOffset2);

		const auto anchorObject = ResolveObject(anchor);

		const std::uint32_t anchorIndex =
			static_cast<std::uint32_t>(anchor) - 1;

		const char* folder =
			"C:\\Users\\Owner\\Documents\\ModFiles\\"
			"LostJudgmentModIdeas\\trace";

		CreateDirectoryA(folder, nullptr);

		const std::string path =
			std::string(folder) + "\\vinyl_disc_objects_" +
			std::to_string(g_dumpCount.load(std::memory_order_relaxed) + 1) +
			".txt";

		std::ofstream out(path, std::ios::out | std::ios::trunc);

		if (!out.is_open())
		{
			HJ::Logger::Error("[RADIAL DISC] dump: cannot open dump file");
			return;
		}

		out
			<< "RADIAL DISC OBJECT DUMP (diagnostic, read-only, build 1d)\n"
			<< "health_ratio=" << health << '\n'
			<< "controller=" << H8(controller) << '\n'
			<< "anchor(+0x180)=" << H8(anchor)
			<< " anchorObj=" << H8(anchorObject)
			<< " anchorIndex=" << IndexHex(anchorIndex)
			<< " anchor2(+0x178)=" << H8(anchor2) << '\n'
			<< "uiManager=" << H8(uiManager)
			<< " capacity=0x"
			<< std::hex << std::uppercase << capacity
			<< " objects=" << H8(objects) << std::dec << '\n'
			<< "layout_ui_layer_all_row=" << kLayoutLayerRow << '\n';

		DumpControllerHandles(out, controller);

		EmitCompactList(out, table);

		EmitSceneTree(out, table);


		std::vector<bool> wantedFlag(table.size(), false);
		std::vector<std::uint32_t> wanted;

		auto addIfSceneMember = [&](std::size_t i)
		{
			if (table[i].sceneMember && !wantedFlag[i])
			{
				wantedFlag[i] = true;
				wanted.push_back(table[i].index);
			}
		};

		for (std::size_t i = 0; i < table.size(); ++i)
			addIfSceneMember(i);

	
		for (std::size_t i = 0; i < table.size(); ++i)
		{
			if (table[i].parentIndex >= 0 &&
				table[table[i].parentIndex].sceneMember &&
				!wantedFlag[i])
			{
				wantedFlag[i] = true;
				wanted.push_back(table[i].index);
			}
		}

		
		for (std::size_t i = 0; i < table.size(); ++i)
		{
			if (table[i].parentIndex < 0 || wantedFlag[i])
				continue;

			const auto& p = table[table[i].parentIndex];

			if (p.parentIndex >= 0 &&
				table[p.parentIndex].sceneMember)
			{
				wantedFlag[i] = true;
				wanted.push_back(table[i].index);
			}
		}

		for (std::size_t i = 0; i < table.size(); ++i)
		{
			if (table[i].cellXSize && !wantedFlag[i])
			{
				wantedFlag[i] = true;
				wanted.push_back(table[i].index);
			}
		}

		std::sort(wanted.begin(), wanted.end());

		out << "\n== SCENE CLOSURE + CELL-HIT WANTED SET ==\n  ";

		for (auto idx : wanted)
			out << IndexHex(idx) << " ";

		out << '\n';

		EmitCellHits(out, table);

		EmitElementTargetScan(out, table);

		EmitFullDumps(out, table, wanted);

		out.close();

		g_lastDumpedAnchor.store(anchor);
		g_dumpDone.store(true);
		g_dumpCount.fetch_add(1);

		std::ostringstream summary;
		summary
			<< "[RADIAL DISC] dump written: trace\\vinyl_disc_objects_"
			<< g_dumpCount.load(std::memory_order_relaxed) + 1
			<< ".txt anchor=" << H8(anchor)
			<< " (index " << IndexHex(anchorIndex) << ")"
			<< " valid=" << table.size()
			<< " scene301="
			<< std::count_if(
					table.begin(), table.end(),
					[](const TableObj& e) { return e.sceneMember; })
			<< " cellHits="
			<< std::count_if(
					table.begin(), table.end(),
					[](const TableObj& e) { return e.cellXSize; });

		HJ::Logger::Info(summary.str());
	}
}

namespace HJ::HUD::RadialDisc
{
	void Initialize()
	{
		g_moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr));
	}

	void Update(float health)
	{
		if (g_dumpCount.load(std::memory_order_relaxed) >= kMaxDumps)
			return;

		const auto controller =
			RadialHealthBridge::GetHealthHudController();

		if (!controller)
			return;

		const auto anchor = ReadU64(controller + kAnchorHandleOffset);

		if (!static_cast<std::uint32_t>(anchor))
			return;

		const std::uintptr_t lastAnchor =
			g_lastDumpedAnchor.load(std::memory_order_relaxed);

		const float lastRatio =
			g_lastDumpedRatio.load(std::memory_order_relaxed);

	
		const bool anchorChanged =
			(lastAnchor != 0 && lastAnchor != anchor);

		const bool ratioChanged =
			std::fabs(health - lastRatio) > 0.05f;

		if (!anchorChanged && !ratioChanged)
			return;

		RunDump(controller, health);

		g_lastDumpedRatio.store(health, std::memory_order_relaxed);
	}

	void Report()
	{
		const int dumps =
			g_dumpCount.load(std::memory_order_relaxed);

		if (dumps == 0)
		{
			HJ::Logger::Info(
				"[RADIAL DISC] no dump yet (no live health HUD seen)");
			return;
		}

		std::ostringstream message;
		message
			<< "[RADIAL DISC] dumps=" << dumps
			<< " last anchor="
			<< H8(g_lastDumpedAnchor.load(std::memory_order_relaxed));

		HJ::Logger::Info(message.str());
	}

	bool IsReady()
	{
		return g_dumpCount.load(std::memory_order_relaxed) > 0;
	}
}