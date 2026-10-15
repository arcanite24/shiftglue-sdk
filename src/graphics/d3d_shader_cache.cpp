// Shader-load cache (Pinyon Shift docs/D3D_RENDERER_RESEARCH.md, DL-0.3
// revisited after DL-2.1): an IM_LOAD that loads microcode at an address and
// size already seen, with the microcode's pages unwritten since it was
// hashed, needs neither the hash nor, when the decoder records inline, the
// shader lookup: the shader the earlier load found is set directly. While
// the decoder and the recorder are split, the cache only supplies the hash.
//
// Validity comes from the page watch (D3dPageWatch below), a write watch on
// the microcode's pages: a guest store, a file read into the page, a
// protection change, a decommit or a release raises the memory system's
// invalidation callbacks, which bump the page's generation. An entry holds
// the generations it was hashed under; a load whose pages still have them
// loads the same microcode.
//
// Inline loads (IM_LOAD_IMMEDIATE) keep the decoder's path: their microcode
// is in the stream, so there is no stable page to watch.

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "d3d_shader_cache.h"

REXCVAR_DEFINE_INT32(gpu_d3d_shader_cache, 2, "GPU",
                     "Keep each IM_LOAD's microcode hash and shader by address and size while a "
                     "write watch shows its pages unwritten, so a repeated load neither hashes "
                     "nor looks the shader up (0: off; 1: verify, hash and look up every load "
                     "and compare; 2: on). Read at each swap")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {

D3dPageWatch::D3dPageWatch(memory::Memory* memory_in) : memory(memory_in) {
  gen.reset(new std::atomic<uint32_t>[kPageCount]);
  armed.reset(new std::atomic<uint8_t>[kPageCount]);
  for (uint32_t i = 0; i < kPageCount; ++i) {
    gen[i].store(0, std::memory_order_relaxed);
    armed[i].store(0, std::memory_order_relaxed);
  }
  handle = memory->RegisterPhysicalMemoryInvalidationCallback(Callback, this);
}

D3dPageWatch::~D3dPageWatch() {
  if (handle) memory->UnregisterPhysicalMemoryInvalidationCallback(handle);
}

void D3dPageWatch::Arm(uint32_t page_first, uint32_t page_count) {
  uint32_t run_first = UINT32_MAX;
  auto flush = [&](uint32_t end) {
    if (run_first == UINT32_MAX) return;
    memory->EnablePhysicalMemoryAccessCallbacks(run_first << kPageShift,
                                                (end - run_first) << kPageShift, true, false);
    ++arms;
    arm_pages += end - run_first;
    run_first = UINT32_MAX;
  };
  const uint32_t page_end = std::min(page_first + page_count, kPageCount);
  for (uint32_t page = page_first; page < page_end; ++page) {
    if (armed[page].load(std::memory_order_relaxed)) {
      flush(page);
      continue;
    }
    armed[page].store(1, std::memory_order_relaxed);
    if (run_first == UINT32_MAX) run_first = page;
  }
  flush(page_end);
}

std::pair<uint32_t, uint32_t> D3dPageWatch::Callback(void* context, uint32_t physical_start,
                                                     uint32_t length, bool exact_range) {
  // On the writing thread, with the global critical region held.
  auto& self = *static_cast<D3dPageWatch*>(context);
  const uint32_t first = std::min(physical_start >> kPageShift, kPageCount - 1);
  const uint32_t last = std::min(
      uint32_t((uint64_t(physical_start) + std::max(length, 1u) - 1) >> kPageShift),
      kPageCount - 1);
  uint32_t dropped = 0;
  for (uint32_t page = first; page <= last; ++page) {
    if (self.armed[page].load(std::memory_order_relaxed)) {
      self.armed[page].store(0, std::memory_order_relaxed);
      ++dropped;
    }
    // After the armed flag: a decoder reading the new generation re-arms.
    self.gen[page].fetch_add(1, std::memory_order_release);
  }
  if (dropped) {
    self.faults.fetch_add(1, std::memory_order_relaxed);
    self.fault_pages.fetch_add(dropped, std::memory_order_relaxed);
  }
  if (exact_range) return {0, UINT32_MAX};
  // The memory system unwatches the intersection of the callbacks' ranges,
  // up to 4 MB around the write: keep it off the other pages we watch, which
  // would otherwise lose their watch without a callback.
  const uint32_t window_first = first & ~uint32_t(1023);
  const uint32_t window_last = std::min(last | 1023u, kPageCount - 1);
  uint32_t lo = first, hi = last;
  while (lo > window_first && !self.armed[lo - 1].load(std::memory_order_relaxed)) --lo;
  while (hi < window_last && !self.armed[hi + 1].load(std::memory_order_relaxed)) ++hi;
  if (lo == window_first && hi == window_last) return {0, UINT32_MAX};
  return {lo << kPageShift, (hi - lo + 1) << kPageShift};
}

D3dPageWatch& CommandProcessor::D3dPageWatchGet() {
  if (!d3d_page_watch_) d3d_page_watch_ = std::make_unique<D3dPageWatch>(memory_);
  return *d3d_page_watch_;
}

bool CommandProcessor::D3dShaderCacheLoad(xenos::ShaderType shader_type, uint32_t addr,
                                          uint32_t size_dwords, const uint32_t* ucode,
                                          uint64_t& hash) {
  using Cache = D3dShaderCache;
  Cache& c = *d3d_shader_cache_;
  Cache::Stats& s = c.stats;
  ++s.loads;
  const uint32_t physical = addr & 0x1FFFFFFFu;
  const uint32_t page_first = physical >> D3dPageWatch::kPageShift;
  const uint32_t page_count =
      size_dwords
          ? ((physical + size_dwords * 4 - 1) >> D3dPageWatch::kPageShift) - page_first + 1
          : 1;
  if (page_count > Cache::kPages) {
    ++s.uncacheable;
    return false;
  }
  const uint32_t slot = ((physical >> 2) * 0x9E3779B1u ^ size_dwords * 0x85EBCA6Bu ^
                         uint32_t(shader_type) * 0xC2B2AE35u) >>
                        (32 - Cache::kEntryBits);
  Cache::Entry& e = c.entries[slot];
  D3dPageWatch& watch = *d3d_page_watch_;
  const bool same_key = e.page_count && e.address == addr && e.dwords == size_dwords &&
                        e.type == uint32_t(shader_type);
  if (same_key && int32_t(e.backoff_until - c.swaps) > 0) {
    ++s.backoff;
    return false;
  }
  bool valid = same_key && e.watched;
  if (valid) {
    for (uint32_t i = 0; i < e.page_count; ++i) {
      if (watch.Gen(e.page_first + i) != e.page_gens[i]) {
        valid = false;
        ++s.stale;
        if (++e.stale >= Cache::kVolatileStale) {
          // Rewritten while in use: watching it costs a fault each time.
          e.stale = 0;
          e.watched = false;
          e.shader = nullptr;
          e.backoff_until = c.swaps + Cache::kBackoffSwaps;
          ++s.backoff;
          return false;
        }
        break;
      }
    }
  }
  const bool verify = d3d_shader_cache_mode_ == 1;
  if (valid && verify) {
    ++s.verified;
    bool match = HashShaderMicrocode(ucode, size_dwords) == e.hash;
    if (match && !record_split_ && e.shader) {
      match = LoadShaderHashed(shader_type, addr, ucode, size_dwords, e.hash) == e.shader;
    }
    if (!match) {
      ++s.mismatches;
      valid = false;
      if (c.logs < 64) {
        ++c.logs;
        REXGPU_INFO("Shader cache mismatch: {:08X}, {} dwords", addr, size_dwords);
      }
    }
  }
  if (!valid) {
    if (!same_key) {
      // First sighting: hash as before and keep a candidate, unwatched.
      const uint64_t new_hash = HashShaderMicrocode(ucode, size_dwords);
      if (!new_hash) {  // the backend hashes for itself
        ++s.uncacheable;
        return false;
      }
      e = {};
      e.address = addr;
      e.dwords = size_dwords;
      e.type = uint32_t(shader_type);
      e.page_first = page_first;
      e.page_count = page_count;
      e.hash = new_hash;
      ++s.candidates;
    } else {
      // Seen before: snapshot, arm, then hash, so a write after the
      // snapshot shows.
      for (uint32_t i = 0; i < page_count; ++i) e.page_gens[i] = watch.Gen(page_first + i);
      watch.Arm(page_first, page_count);
      const uint64_t new_hash = HashShaderMicrocode(ucode, size_dwords);
      if (new_hash != e.hash) e.shader = nullptr;
      e.hash = new_hash;
      e.watched = true;
      ++s.fills;
    }
  } else {
    ++s.hits;
  }
  // The shader is looked up on the decoder only when it records inline (the
  // recorder owns the pipeline cache while split).
  if (!record_split_ && !e.shader) {
    e.shader = LoadShaderHashed(shader_type, addr, ucode, size_dwords, e.hash);
  }
  if (verify) return false;  // the decoder hashes and looks up as usual
  hash = e.hash;
  if (record_split_) return false;  // recorded with the hash
  Shader*& active =
      shader_type == xenos::ShaderType::kVertex ? active_vertex_shader_ : active_pixel_shader_;
  s.same += active == e.shader;
  active = e.shader;
  return true;
}

void CommandProcessor::D3dShaderCacheReset() {
  if (!d3d_shader_cache_) return;
  for (uint32_t i = 0; i < D3dShaderCache::kEntries; ++i) d3d_shader_cache_->entries[i] = {};
}

void CommandProcessor::D3dShaderCacheSwap() {
  d3d_shader_cache_mode_ = uint32_t(REXCVAR_GET(gpu_d3d_shader_cache));
  if (!d3d_shader_cache_mode_ && !d3d_shader_cache_) return;
  if (d3d_shader_cache_mode_ && !d3d_shader_cache_) {
    D3dPageWatchGet();
    d3d_shader_cache_ = std::make_unique<D3dShaderCache>();
  }
  D3dShaderCache& c = *d3d_shader_cache_;
  ++c.swaps;
  if (++c.sweep_swaps < D3dShaderCache::kSweepSwaps) return;
  c.sweep_swaps = 0;
  for (uint32_t i = 0; i < D3dShaderCache::kEntries; ++i) c.entries[i].stale = 0;
  const auto& t = c.stats;
  if (t.loads) {
    std::string watch;
    if (!d3d_list_watch_ && d3d_page_watch_) {
      // The list compiler logs (and resets) these when it watches too.
      D3dPageWatch& w = *d3d_page_watch_;
      watch = fmt::format("; watch: {} arms ({} pages), {} writes dropped a watch ({} pages)",
                          w.arms, w.arm_pages, w.faults.exchange(0, std::memory_order_relaxed),
                          w.fault_pages.exchange(0, std::memory_order_relaxed));
      w.arms = w.arm_pages = 0;
    }
    REXGPU_INFO(
        "Shader cache ({} swaps, mode {}): {} loads, {} hits ({} already active), {} first "
        "sightings, {} fills, {} stale, {} in backoff, {} uncacheable; verify: {} compared, {} "
        "mismatches{}",
        D3dShaderCache::kSweepSwaps, d3d_shader_cache_mode_, t.loads, t.hits, t.same,
        t.candidates, t.fills, t.stale, t.backoff, t.uncacheable, t.verified, t.mismatches,
        watch);
  }
  c.stats = {};
}

}  // namespace rex::graphics
