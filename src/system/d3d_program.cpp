#include <rex/system/d3d_program.h>

#include <atomic>

namespace rex::system::d3d_program {
namespace {

std::atomic<uint32_t> g_mode{0};
std::atomic<uint32_t> g_swap{1};

// Marks: open addressing, one relaxed 64-bit word each, physical (high) and
// the swap of the latest mark (low). Lock-free: the decoder looks up every
// nested buffer it enters. A full table drops new marks.
constexpr uint32_t kSlotBits = 12;
constexpr uint32_t kSlots = 1u << kSlotBits;
constexpr uint32_t kProbes = 16;
constexpr uint32_t kMarkSwaps = 8;
std::atomic<uint64_t> g_slots[kSlots];

uint32_t SlotOf(uint32_t physical) { return (physical * 0x9E3779B1u) >> (32 - kSlotBits); }

}  // namespace

uint32_t Mode() { return g_mode.load(std::memory_order_relaxed); }
void SetMode(uint32_t mode) { g_mode.store(mode, std::memory_order_relaxed); }
void Swap() { g_swap.fetch_add(1, std::memory_order_relaxed); }

void MarkList(uint32_t physical) {
  physical &= 0x1FFFFFFFu;
  if (!physical) return;
  const uint64_t value = uint64_t(physical) << 32 | g_swap.load(std::memory_order_relaxed);
  const uint32_t slot = SlotOf(physical);
  for (uint32_t probe = 0; probe < kProbes; ++probe) {
    std::atomic<uint64_t>& entry = g_slots[(slot + probe) & (kSlots - 1)];
    uint64_t current = entry.load(std::memory_order_relaxed);
    while (!current || uint32_t(current >> 32) == physical) {
      if (entry.compare_exchange_weak(current, value, std::memory_order_relaxed)) return;
    }
  }
}

bool IsList(uint32_t physical) {
  physical &= 0x1FFFFFFFu;
  const uint32_t swap = g_swap.load(std::memory_order_relaxed);
  const uint32_t slot = SlotOf(physical);
  for (uint32_t probe = 0; probe < kProbes; ++probe) {
    const uint64_t current = g_slots[(slot + probe) & (kSlots - 1)].load(std::memory_order_relaxed);
    if (!current) return false;
    if (uint32_t(current >> 32) == physical) return swap - uint32_t(current) <= kMarkSwaps;
  }
  return false;
}

}  // namespace rex::system::d3d_program
