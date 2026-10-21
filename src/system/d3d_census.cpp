#include <rex/system/d3d_census.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rex::system::d3d_census {
namespace {

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_lean{false};
std::atomic<uint32_t> g_frame{0};

// A mark older than this many guest frames is ignored: the guest runs at
// most one or two frames ahead of the decoder, and segment memory is reused.
constexpr uint32_t kMarkFrames = 4;

// Packet marks: direct-mapped, lossy (a collision drops the older mark), one
// relaxed 64-bit word each: physical >> 2 (27 bits), source (5 bits), frame
// (32 bits). Lock-free so the decoder can look up every packet.
constexpr uint32_t kMarkBits = 20;
std::unique_ptr<std::atomic<uint64_t>[]> g_marks;
std::once_flag g_marks_once;

uint32_t MarkIndex(uint32_t word) {
  return uint32_t((uint64_t(word) * UINT64_C(0x9E3779B97F4A7C15)) >> (64 - kMarkBits));
}

struct Segment {
  uint32_t bytes;
  uint32_t frame;
  uint8_t mode;
};
struct ListBuffer {
  uint32_t bytes;
  uint32_t object;
  uint8_t origin;
  uint8_t state;
};
struct ObjectWrites {
  uint32_t counts[kWriterClassCount] = {};
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Segment> g_segments;
std::unordered_map<uint32_t, ListBuffer> g_lists;
std::unordered_map<uint32_t, ObjectWrites> g_object_writes;
uint32_t g_segments_pruned_frame = 0;

struct WriterRange {
  uint32_t begin, end;
  uint8_t writer_class;
};
std::mutex g_writer_mutex;
std::vector<WriterRange> g_writer_ranges;
WriterWindow g_writer_window;
std::unordered_map<uint32_t, uint64_t> g_other_lrs;

std::mutex g_decoder_mutex;
DecoderWindow g_decoder_window;

}  // namespace

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

void SetEnabled(bool enabled) {
  if (enabled) {
    std::call_once(g_marks_once, [] {
      g_marks.reset(new std::atomic<uint64_t>[size_t(1) << kMarkBits]);
      for (size_t i = 0; i < (size_t(1) << kMarkBits); ++i) {
        g_marks[i].store(0, std::memory_order_relaxed);
      }
    });
  }
  g_enabled.store(enabled, std::memory_order_release);
}

bool Lean() { return g_lean.load(std::memory_order_relaxed); }
void SetLean(bool lean) { g_lean.store(lean, std::memory_order_relaxed); }

uint32_t Frame() { return g_frame.load(std::memory_order_relaxed); }
void SetFrame(uint32_t frame) { g_frame.store(frame, std::memory_order_relaxed); }

void MarkPacket(uint32_t physical, uint8_t source) {
  if (!g_marks) return;
  const uint32_t word = (physical & 0x1FFFFFFFu) >> 2;
  const uint64_t value = uint64_t(word) | (uint64_t(source & 31u) << 27) |
                         (uint64_t(Frame()) << 32);
  g_marks[MarkIndex(word)].store(value, std::memory_order_relaxed);
}

uint8_t FindPacket(uint32_t physical, bool any_age) {
  if (!g_marks) return kSourceNone;
  const uint32_t word = (physical & 0x1FFFFFFFu) >> 2;
  const uint64_t value = g_marks[MarkIndex(word)].load(std::memory_order_relaxed);
  if (!value || uint32_t(value & 0x7FFFFFFu) != word) return kSourceNone;
  const uint32_t frame = uint32_t(value >> 32);
  if (!any_age && Frame() - frame > kMarkFrames) return kSourceNone;
  return uint8_t((value >> 27) & 31u);
}

void MarkSegment(uint32_t physical, uint32_t bytes, uint8_t mode) {
  const uint32_t frame = Frame();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_segments[physical & 0x1FFFFFFFu] = {bytes, frame, mode};
  // Ring memory is reused: drop starts not seen for a while, now and then.
  if (frame - g_segments_pruned_frame > 120) {
    g_segments_pruned_frame = frame;
    for (auto it = g_segments.begin(); it != g_segments.end();) {
      it = frame - it->second.frame > 60 ? g_segments.erase(it) : std::next(it);
    }
  }
}

uint8_t FindSegment(uint32_t physical) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_segments.find(physical & 0x1FFFFFFFu);
  if (it == g_segments.end() || Frame() - it->second.frame > kMarkFrames) {
    return kSegmentUnknown;
  }
  return it->second.mode;
}

void MarkListBuffer(uint32_t physical, uint32_t bytes, uint32_t object, uint8_t origin,
                    uint8_t state) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_lists[physical & 0x1FFFFFFFu] = {bytes, object, origin, state};
}

bool FindListBuffer(uint32_t physical, uint32_t* object, uint8_t* origin, uint8_t* state) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_lists.find(physical & 0x1FFFFFFFu);
  if (it == g_lists.end()) return false;
  if (object) *object = it->second.object;
  if (origin) *origin = it->second.origin;
  if (state) *state = it->second.state;
  return true;
}

void TakeListWrites(uint32_t object, uint32_t (&counts)[kWriterClassCount]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_object_writes.find(object);
  if (it == g_object_writes.end()) {
    std::fill(std::begin(counts), std::end(counts), 0u);
    return;
  }
  std::copy(std::begin(it->second.counts), std::end(it->second.counts), std::begin(counts));
  g_object_writes.erase(it);
}

void AddWriterRange(uint32_t begin, uint32_t end, uint8_t writer_class) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  for (const WriterRange& range : g_writer_ranges) {
    if (range.begin == begin && range.end == end) return;
  }
  g_writer_ranges.push_back({begin, end, writer_class});
}

uint8_t ClassifyWriter(uint32_t lr) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  for (const WriterRange& range : g_writer_ranges) {
    if (lr >= range.begin && lr < range.end) return range.writer_class;
  }
  return kWriterOther;
}

void NoteBufferWrite(uint32_t buffer_physical, bool list, uint8_t writer_class, uint32_t lr) {
  if (writer_class >= kWriterClassCount) writer_class = kWriterOther;
  bool known_list = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_lists.find(buffer_physical & 0x1FFFFFFFu);
    if (it != g_lists.end()) {
      known_list = true;
      ++g_object_writes[it->second.object].counts[writer_class];
    }
  }
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  ++g_writer_window.writes[list ? 0 : 1][writer_class];
  if (known_list) ++g_writer_window.list_object_writes[writer_class];
  if (writer_class == kWriterOther && g_other_lrs.size() < 4096) ++g_other_lrs[lr];
}

void TakeWriterWindow(WriterWindow* out) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  *out = g_writer_window;
  std::vector<std::pair<uint32_t, uint64_t>> lrs(g_other_lrs.begin(), g_other_lrs.end());
  std::sort(lrs.begin(), lrs.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (size_t i = 0; i < lrs.size() && i < std::size(out->other_lr); ++i) {
    out->other_lr[i] = lrs[i].first;
    out->other_lr_count[i] = lrs[i].second;
  }
  g_writer_window = {};
  g_other_lrs.clear();
}

void AddDecoderWindow(const DecoderWindow& window) {
  std::lock_guard<std::mutex> lock(g_decoder_mutex);
  DecoderWindow& sum = g_decoder_window;
  sum.swaps += window.swaps;
  for (size_t i = 0; i < kContextCount; ++i) sum.ibs[i] += window.ibs[i];
  for (size_t i = 0; i < kDrawClassCount; ++i) {
    sum.drawn[i] += window.drawn[i];
    sum.decode_ns[i] += window.decode_ns[i];
  }
  for (size_t s = 0; s < kSourceCount; ++s) {
    for (size_t c = 0; c < kContextCount; ++c) {
      sum.drawn_by_source[s][c] += window.drawn_by_source[s][c];
    }
  }
  for (size_t i = 0; i < kListStateCount; ++i) {
    sum.list_drawn_by_state[i] += window.list_drawn_by_state[i];
  }
  for (size_t i = 0; i < std::size(sum.list_drawn_by_origin); ++i) {
    sum.list_drawn_by_origin[i] += window.list_drawn_by_origin[i];
  }
  for (size_t i = 0; i < 3; ++i) sum.spans[i] += window.spans[i];
  sum.marks_found += window.marks_found;
  sum.gap_marks_found += window.gap_marks_found;
}

void TakeDecoderWindow(DecoderWindow* out) {
  std::lock_guard<std::mutex> lock(g_decoder_mutex);
  *out = g_decoder_window;
  g_decoder_window = {};
}

namespace {
std::mutex g_list_decoded_mutex;
std::unordered_map<uint32_t, ListDecoded> g_list_decoded;
}  // namespace

void AddListDecoded(uint32_t object, const ListDecoded& counts) {
  std::lock_guard<std::mutex> lock(g_list_decoded_mutex);
  ListDecoded& sum = g_list_decoded[object];
  sum.draws += counts.draws;
  sum.copies += counts.copies;
  sum.events += counts.events;
  sum.memory_writes += counts.memory_writes;
  sum.waits += counts.waits;
  sum.constants += counts.constants;
  sum.shader_loads += counts.shader_loads;
}

void TakeListDecoded(std::vector<std::pair<uint32_t, ListDecoded>>* out) {
  std::lock_guard<std::mutex> lock(g_list_decoded_mutex);
  out->assign(g_list_decoded.begin(), g_list_decoded.end());
  g_list_decoded.clear();
}

}  // namespace rex::system::d3d_census
