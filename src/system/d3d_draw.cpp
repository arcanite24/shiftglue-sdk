#include <rex/system/d3d_draw.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace rex::system::d3d_draw {
namespace {

// The guest runs at most a frame or two ahead of the decoder; menus make
// about 220 qualifying draws a frame.
constexpr uint32_t kSlotCount = 1024;

struct Slot {
  // 0 while being written; the id once complete.
  std::atomic<uint32_t> id{0};
  Description description;
};

std::unique_ptr<Slot[]> g_slots;
std::once_flag g_slots_once;
std::atomic<uint32_t> g_next_id{0};
std::atomic<uint32_t> g_published{0};

Slot* Slots() {
  std::call_once(g_slots_once, [] { g_slots.reset(new Slot[kSlotCount]); });
  return g_slots.get();
}

constexpr size_t kHeaderBytes = offsetof(Description, words);

}  // namespace

uint32_t NextId() {
  uint32_t id = g_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!id) id = g_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
  return id;
}

void Publish(const Description& description) {
  Slot& slot = Slots()[description.id % kSlotCount];
  slot.id.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  std::memcpy(&slot.description, &description,
              kHeaderBytes + size_t(description.word_count) * sizeof(uint32_t));
  slot.id.store(description.id, std::memory_order_release);
  uint32_t published = g_published.load(std::memory_order_relaxed);
  while (int32_t(description.id - published) > 0 &&
         !g_published.compare_exchange_weak(published, description.id,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
  }
}

bool Fetch(uint32_t id, Description* out) {
  if (!id) return false;
  Slot& slot = Slots()[id % kSlotCount];
  // A marker can reach the decoder before its call returns when the device
  // kicks a segment inside the call: wait briefly for the publish.
  if (int32_t(id - g_published.load(std::memory_order_acquire)) > 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    while (int32_t(id - g_published.load(std::memory_order_acquire)) > 0 &&
           slot.id.load(std::memory_order_acquire) != id) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::yield();
    }
  }
  if (slot.id.load(std::memory_order_acquire) != id) return false;
  std::memcpy(out, &slot.description, kHeaderBytes);
  if (out->word_count > kMaxWords) return false;
  std::memcpy(out->words, slot.description.words, size_t(out->word_count) * sizeof(uint32_t));
  std::atomic_thread_fence(std::memory_order_acquire);
  return slot.id.load(std::memory_order_relaxed) == id && out->id == id;
}

}  // namespace rex::system::d3d_draw

// --- DL-2.1 replacement arena ---

namespace rex::system::d3d_draw {
namespace {

constexpr uint32_t kChunkBytes = 512 * 1024;
constexpr uint32_t kChunkCount = 32;  // 16 MB
// A chunk is reused once the decoder has fetched a description this many
// guest frames newer than the chunk's last writer.
constexpr uint32_t kChunkFramesLive = 2;

struct Arena {
  std::mutex mutex;
  std::unique_ptr<uint8_t[]> bytes;
  uint64_t cursor = 0;  // bytes allocated, monotonic
  uint32_t chunk_frame[kChunkCount] = {};
  bool chunk_used[kChunkCount] = {};
};
Arena g_arena;
std::atomic<uint32_t> g_slice_mode{0};
std::atomic<uint32_t> g_guest_frame{0};
// The newest guest frame the decoder has fetched a description of.
std::atomic<uint32_t> g_decoder_frame{0};

}  // namespace

uint32_t GetSliceMode() { return g_slice_mode.load(std::memory_order_relaxed); }
void SetSliceMode(uint32_t mode) { g_slice_mode.store(mode, std::memory_order_relaxed); }

void GuestFrame() { g_guest_frame.fetch_add(1, std::memory_order_relaxed); }

uint32_t PublishArena(const Description& description) {
  const size_t bytes =
      (kHeaderBytes + size_t(description.word_count) * sizeof(uint32_t) + 15) & ~size_t(15);
  if (bytes > kChunkBytes) return 0;
  Arena& arena = g_arena;
  std::lock_guard<std::mutex> lock(arena.mutex);
  if (!arena.bytes) arena.bytes.reset(new uint8_t[size_t(kChunkBytes) * kChunkCount]);
  uint64_t position = arena.cursor;
  uint64_t chunk = position / kChunkBytes;
  if (position % kChunkBytes + bytes > kChunkBytes) {
    ++chunk;
    position = chunk * kChunkBytes;
  }
  const uint32_t slot = uint32_t(chunk % kChunkCount);
  const uint32_t guest_frame = g_guest_frame.load(std::memory_order_relaxed);
  if (position % kChunkBytes == 0 && arena.chunk_used[slot]) {
    // Entering a chunk an earlier lap wrote: only once the decoder is past it.
    const uint32_t decoder_frame = g_decoder_frame.load(std::memory_order_acquire);
    if (int32_t(decoder_frame - (arena.chunk_frame[slot] + kChunkFramesLive)) < 0) return 0;
  }
  arena.chunk_used[slot] = true;
  arena.chunk_frame[slot] = guest_frame;
  arena.cursor = position + bytes;
  const uint32_t offset = uint32_t(position % (uint64_t(kChunkBytes) * kChunkCount));
  auto* stored = reinterpret_cast<Description*>(arena.bytes.get() + offset);
  std::memcpy(stored, &description,
              kHeaderBytes + size_t(description.word_count) * sizeof(uint32_t));
  stored->frame = guest_frame;
  std::atomic_thread_fence(std::memory_order_release);
  return offset + 1;
}

const Description* FetchArena(uint32_t offset, uint32_t id) {
  std::atomic_thread_fence(std::memory_order_acquire);
  uint8_t* bytes = g_arena.bytes.get();
  if (!bytes || offset >= kChunkBytes * kChunkCount || (offset & 15)) return nullptr;
  const auto* description = reinterpret_cast<const Description*>(bytes + offset);
  if (description->id != id || description->word_count > kMaxWords ||
      offset % kChunkBytes + kHeaderBytes + description->word_count * sizeof(uint32_t) >
          kChunkBytes) {
    return nullptr;
  }
  // Only the decoder thread stores it.
  if (int32_t(description->frame - g_decoder_frame.load(std::memory_order_relaxed)) > 0) {
    g_decoder_frame.store(description->frame, std::memory_order_release);
  }
  return description;
}

}  // namespace rex::system::d3d_draw
