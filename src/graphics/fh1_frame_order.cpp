#include <rex/graphics/d3d12/fh1_frame_census.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(fh1_frame_census_ordered, false, "Diagnostics",
                    "Vulkan census: retain guest draw order and selected pipelines per rendering instance")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_frame_census_ordered_path, "", "Diagnostics",
                      "Exclusive JSON Lines output for the ordered Vulkan frame census")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_frame_census_ordered_every, 1, "Diagnostics",
                     "Ordered frame census: write two consecutive frames in this many (1 writes every frame)")
    .range(1, 100000);

namespace rex::graphics::d3d12 {
namespace {
struct Draw {
  uint64_t pass;
  uintptr_t pipeline;
  Fh1FrameCensus::OrderedDrawInputs inputs;
};
struct OrderedCensus {
  FILE* file = nullptr;
  uint64_t frame = UINT64_MAX, overflow = 0;
  std::vector<Draw> draws;
  char buffer[1024 * 1024];
};
OrderedCensus& State() { static auto* state = new OrderedCensus; return *state; }
constexpr size_t kMaxDraws = 65536;
}

bool Fh1FrameCensus::OrderedEnabled() {
  static const bool enabled = [] {
    if (!REXCVAR_GET(fh1_frame_census_ordered)) return false;
    auto& state = State();
    const auto path = REXCVAR_GET(fh1_frame_census_ordered_path);
    if (!path.empty()) state.file = std::fopen(path.c_str(), "wbx");
    if (!state.file) {
      REXLOG_ERROR("Ordered frame census cannot exclusively create its output");
      return false;
    }
    std::setvbuf(state.file, state.buffer, _IOFBF, sizeof(state.buffer));
    state.draws.reserve(8192);
    return true;
  }();
  return enabled;
}

void Fh1FrameCensus::ObserveOrderedDraw(uint64_t frame, uint64_t pass, const void* pipeline,
                                        const OrderedDrawInputs& inputs) {
  auto& state = State();
  // A missing swap is an invalid census rather than an unbounded buffer.
  if (state.frame != UINT64_MAX && state.frame != frame) ++state.overflow;
  state.frame = frame;
  if (!pipeline || !pass) ++state.overflow;
  if (state.draws.size() == kMaxDraws) { ++state.overflow; return; }
  state.draws.push_back({pass, reinterpret_cast<uintptr_t>(pipeline), inputs});
}

void Fh1FrameCensus::ObserveOrderedSwap(uint64_t frame, uint64_t output_frame) {
  auto& state = State();
  if (state.frame != UINT64_MAX && state.frame != frame) ++state.overflow;
  // Frames are written in consecutive pairs, so a reader can compare a draw
  // with the same draw a frame earlier.
  const uint64_t every = uint64_t(REXCVAR_GET(fh1_frame_census_ordered_every));
  if (every > 1 && output_frame % every >= 2) {
    state.draws.clear(); state.overflow = 0; state.frame = UINT64_MAX;
    return;
  }
  fmt::memory_buffer line;
  auto out = std::back_inserter(line);
  fmt::format_to(out,
      "{{\"schema\":\"pinyon-shift.fh1-frame-order.v3\",\"backend\":\"vulkan\","
      "\"identity\":\"process-local-pipeline-cache-entry\",\"frame\":{},"
      "\"output_frame\":{},\"overflow\":{},\"draws\":[", frame, output_frame, state.overflow);
  bool first = true;
  for (const auto& draw : state.draws) {
    fmt::format_to(out, "{}[{},\"{:X}\",\"{:X}\",\"{:X}\",{},{},{},\"{:X}\"]", first ? "" : ",",
                   draw.pass, draw.pipeline, draw.inputs.textures, draw.inputs.vertices,
                   draw.inputs.index_base, draw.inputs.blend, draw.inputs.list,
                   draw.inputs.dynamic_state);
    first = false;
  }
  fmt::format_to(out, "]}}\n");
  if (std::fwrite(line.data(), 1, line.size(), state.file) != line.size() ||
      std::fflush(state.file)) {
    REXLOG_ERROR("Ordered frame census write failed");
    std::abort();
  }
  state.draws.clear(); state.overflow = 0; state.frame = UINT64_MAX;
}
}  // namespace rex::graphics::d3d12
