#pragma once

// The page watch and the shader-load cache (d3d_shader_cache.cpp). Private
// to command_processor.cpp, d3d_list_compiler.cpp and d3d_shader_cache.cpp.

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include <rex/graphics/command_processor.h>

namespace rex::memory {
class Memory;
}

namespace rex::graphics {

// Guest writes into watched physical pages, through the memory system's
// invalidation callbacks (the same write watch the shared memory uses, so
// file loads, protection changes and kernel writes report too). A watched
// page is read-only to the guest until it is written; the write bumps the
// page's generation and drops the watch. Whoever depends on a page takes a
// snapshot of its generation, then arms it, then reads it: a later write
// shows as a changed generation. Shared by the list compiler and the shader
// cache.
struct D3dPageWatch {
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPageCount = 0x20000000u >> kPageShift;
  std::unique_ptr<std::atomic<uint32_t>[]> gen;
  std::unique_ptr<std::atomic<uint8_t>[]> armed;
  memory::Memory* memory = nullptr;
  void* handle = nullptr;
  // Written by guest threads in the callback.
  std::atomic<uint64_t> faults{0};       // callbacks that dropped one of our watches
  std::atomic<uint64_t> fault_pages{0};  // pages whose watch they dropped
  // Decoder side.
  uint64_t arms = 0, arm_pages = 0;

  explicit D3dPageWatch(memory::Memory* memory);
  ~D3dPageWatch();
  uint32_t Gen(uint32_t page) const { return gen[page].load(std::memory_order_acquire); }
  // Watches [page_first, page_first + page_count) where it is not watched yet.
  void Arm(uint32_t page_first, uint32_t page_count);
  static std::pair<uint32_t, uint32_t> Callback(void* context, uint32_t physical_start,
                                                uint32_t length, bool exact_range);
};

// Shader-load cache (gpu_d3d_shader_cache): an IM_LOAD's microcode hash and,
// when the decoder records inline, its shader, by (type, address, dwords),
// valid while the page watch shows the microcode's pages unwritten.
//
// A key is watched only from its second load: the first load hashes as
// before and leaves a candidate, so microcode loaded once (most of loading)
// arms no watch and takes no write fault. A key whose pages are found
// written twice is left uncached for a while (kBackoffSwaps).
struct CommandProcessor::D3dShaderCache {
  struct Entry {
    uint32_t address = 0;
    uint32_t dwords = 0;
    uint32_t type = 0;
    uint32_t page_first = 0;
    uint32_t page_count = 0;  // 0: empty
    uint32_t page_gens[5] = {};  // kPages
    uint64_t hash = 0;
    Shader* shader = nullptr;  // inline decoding only; nullptr until looked up
    uint32_t stale = 0;        // times found written
    uint32_t backoff_until = 0;  // swap; not cached until then
    bool watched = false;      // false: a candidate (seen once, not armed)
  };
  static constexpr uint32_t kEntryBits = 12;
  static constexpr uint32_t kEntries = 1u << kEntryBits;
  static constexpr uint32_t kPages = 5;
  static constexpr uint32_t kVolatileStale = 2;
  static constexpr uint32_t kBackoffSwaps = 600;
  static constexpr uint32_t kSweepSwaps = 600;
  std::unique_ptr<Entry[]> entries{new Entry[kEntries]};
  uint32_t swaps = 0;
  uint32_t sweep_swaps = 0;
  uint32_t logs = 0;
  struct Stats {
    uint64_t loads = 0;
    uint64_t hits = 0;
    uint64_t same = 0;        // hit, and already the active shader
    uint64_t candidates = 0;  // first sightings (hashed, not armed)
    uint64_t fills = 0;       // armed and hashed
    uint64_t stale = 0;       // entry found, pages written since
    uint64_t backoff = 0;     // loads of a key in backoff
    uint64_t uncacheable = 0; // more than kPages pages, or no hash
    uint64_t verified = 0;
    uint64_t mismatches = 0;  // hash or shader differs
  } stats;
};

}  // namespace rex::graphics
