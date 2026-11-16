#include <rex/system/fh1_occlusion_grid.h>

#include <array>
#include <atomic>

namespace rex::system {

namespace {
std::array<Fh1OcclusionGrid, 4> g_slots;
std::atomic<int32_t> g_latest{-1};
int32_t g_next = 0;
std::atomic<uint64_t> g_tested{0};
std::atomic<uint64_t> g_culled{0};
}  // namespace

void PublishFh1OcclusionGrid(const Fh1OcclusionGrid& grid) {
  const int32_t slot = g_next;
  g_next = (g_next + 1) % int32_t(g_slots.size());
  g_slots[slot] = grid;
  g_latest.store(slot, std::memory_order_release);
}

void ClearFh1OcclusionGrid() { g_latest.store(-1, std::memory_order_release); }

const Fh1OcclusionGrid* LatestFh1OcclusionGrid() {
  const int32_t slot = g_latest.load(std::memory_order_acquire);
  return slot < 0 ? nullptr : &g_slots[slot];
}

void CountFh1OcclusionTests(uint32_t tested, uint32_t culled) {
  g_tested.fetch_add(tested, std::memory_order_relaxed);
  g_culled.fetch_add(culled, std::memory_order_relaxed);
}

void TakeFh1OcclusionCounts(uint64_t& tested, uint64_t& culled) {
  tested = g_tested.exchange(0, std::memory_order_relaxed);
  culled = g_culled.exchange(0, std::memory_order_relaxed);
}

}  // namespace rex::system
