#include "d3d_census_decoder.h"

#include <chrono>
#include <unordered_map>
#include <vector>

#include <rex/perf/counter.h>
#include <rex/system/d3d_census.h>

namespace rex::graphics::d3d_census_decoder {

namespace census = rex::system::d3d_census;

bool g_active = false;

namespace {

constexpr uint8_t kNoTimeClass = 0xFF;

struct Level {
  uint32_t ptr;
  uint32_t depth;
  uint32_t object;
  uint8_t context;
  uint8_t source;
  uint8_t origin;
  uint8_t state;
  bool visible;
};

struct State {
  std::vector<Level> stack;
  census::DecoderWindow window;
  // The source at the end of the last outermost segment: a call whose
  // packets did not fit continues in the next segment, without a mark.
  uint8_t carry_source = census::kSourceNone;
  // Draws since the last draw-entry or internal mark.
  uint32_t span_draws = 0;
  bool span_open = false;
  uint8_t time_class = kNoTimeClass;
  int64_t time_start_ns = 0;
  // census::Lean(), read when an outermost buffer starts.
  bool lean = false;
  // Per command buffer this frame (NA-2.2), flushed at the swap.
  std::unordered_map<uint32_t, census::ListDecoded> lists;
};
State state;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool IsDrawSource(uint8_t source) {
  return (source >= census::kSourceDrawEntry && source <= census::kSourceDrawEntryLast) ||
         source == census::kSourceInternal;
}

uint8_t DrawClassOf(const Level& level) {
  switch (level.context) {
    case census::kContextList:
      return census::kDrawList;
    case census::kContextD3dBuffer:
      return census::kDrawD3dBuffer;
    case census::kContextSegmentRing:
    case census::kContextSegmentTiling:
      if (level.source == census::kSourceInternal) return census::kDrawInternal;
      if (level.source >= census::kSourceDrawEntry &&
          level.source <= census::kSourceDrawEntryLast) {
        return level.context == census::kContextSegmentTiling ? census::kDrawTiledImmediate
                                                              : census::kDrawImmediate;
      }
      return census::kDrawUnattributed;
    default:
      return census::kDrawUnknownBuffer;
  }
}

// What the decoder's time is spent on while in `level`: setup packets a run
// writes into its calling segment count with the lists.
uint8_t TimeClassOf(const Level& level) {
  if ((level.context == census::kContextSegmentRing ||
       level.context == census::kContextSegmentTiling) &&
      (level.source == census::kSourceListRun || level.source == census::kSourceAsyncRun)) {
    return census::kDrawList;
  }
  return DrawClassOf(level);
}

// The DL-0.1 "visible" rule (game side LogWindow): draw calls and lists
// that are unchanged or changed only by fixups; everything else would need
// the PM4 fallback.
bool VisibleOf(const Level& level) {
  switch (DrawClassOf(level)) {
    case census::kDrawImmediate:
    case census::kDrawTiledImmediate:
    case census::kDrawInternal:
      return true;
    case census::kDrawList:
      return level.state != census::kListOtherWrite &&
             level.state != census::kListChangedNoFixup;
    default:
      return false;
  }
}

void SwitchTime(uint8_t time_class) {
  if (state.lean) return;
  const int64_t now = NowNs();
  if (state.time_class != kNoTimeClass) {
    state.window.decode_ns[state.time_class] += uint64_t(now - state.time_start_ns);
  }
  state.time_class = time_class;
  state.time_start_ns = now;
}

void CloseSpan() {
  if (state.span_open) {
    ++state.window.spans[state.span_draws < 2 ? state.span_draws : 2];
  }
  state.span_open = false;
  state.span_draws = 0;
}

void SetSource(Level& level, uint8_t source) {
  if (IsDrawSource(source)) {
    CloseSpan();
    state.span_open = true;
  } else if (IsDrawSource(level.source)) {
    CloseSpan();
  }
  level.source = source;
  level.visible = VisibleOf(level);
}

}  // namespace

void IbBegin(uint32_t ptr, uint32_t count, uint32_t depth) {
  (void)count;
  if (state.stack.empty() && !census::Enabled()) {
    return;
  }
  if (state.stack.empty()) state.lean = census::Lean();
  const uint32_t physical = ptr & 0x1FFFFFFFu;
  Level level{physical, depth, 0, census::kContextNestedUnknown, census::kSourceNone, 0,
              census::kListFirstRun, false};
  if (census::FindListBuffer(physical, &level.object, &level.origin, &level.state)) {
    level.context = (level.origin & census::kOriginD3dBuffer) ? census::kContextD3dBuffer
                                                              : census::kContextList;
  } else if (depth <= 1) {
    switch (census::FindSegment(physical)) {
      case census::kSegmentRing:
        level.context = census::kContextSegmentRing;
        break;
      case census::kSegmentTiling:
        level.context = census::kContextSegmentTiling;
        break;
      case census::kSegmentRecording:
        level.context = census::kContextSegmentRecording;
        break;
      default:
        level.context = census::kContextSegmentUnknown;
        break;
    }
    // A call that closed the segment before its first packet left its mark
    // in the alignment gap before this one (segments start 32-byte aligned).
    uint8_t source = census::kSourceNone;
    for (uint32_t back = 4; back <= 28 && source == census::kSourceNone; back += 4) {
      source = census::FindPacket(physical - back);
    }
    if (source != census::kSourceNone) {
      ++state.window.gap_marks_found;
    } else {
      source = state.carry_source;
    }
    level.source = source;
    if (IsDrawSource(source)) state.span_open = true;
  }
  level.visible = VisibleOf(level);
  ++state.window.ibs[level.context];
  state.stack.push_back(level);
  g_active = true;
  SwitchTime(TimeClassOf(state.stack.back()));
}

void IbEnd(uint32_t depth) {
  if (state.stack.empty() || state.stack.back().depth != depth) {
    return;
  }
  const Level level = state.stack.back();
  state.stack.pop_back();
  if (level.depth <= 1) {
    state.carry_source = level.source;
  }
  if (state.stack.empty()) {
    SwitchTime(kNoTimeClass);
    g_active = false;
  } else {
    SwitchTime(TimeClassOf(state.stack.back()));
  }
}

// The innermost command buffer being decoded, or null.
census::ListDecoded* InnermostList() {
  for (auto it = state.stack.rbegin(); it != state.stack.rend(); ++it) {
    if (it->context == census::kContextList) return &state.lists[it->object];
  }
  return nullptr;
}

void Packet(uint32_t offset, uint32_t packet) {
  if (state.stack.empty()) return;
  Level& level = state.stack.back();
  if (!state.lean && (packet >> 30) == 3) {
    if (census::ListDecoded* list = InnermostList()) {
      switch ((packet >> 8) & 0x7F) {
        case 0x46: case 0x58: case 0x5A: case 0x5B:  // EVENT_WRITE family
          ++list->events;
          break;
        case 0x3D: case 0x3E: case 0x45:  // MEM_WRITE, REG_TO_MEM, COND_WRITE
          ++list->memory_writes;
          break;
        case 0x3C:  // WAIT_REG_MEM
          ++list->waits;
          break;
        case 0x2D: case 0x55: case 0x2F:  // SET_CONSTANT(2), LOAD_ALU_CONSTANT
          ++list->constants;
          break;
        case 0x27: case 0x2B:  // IM_LOAD, IM_LOAD_IMMEDIATE
          ++list->shader_loads;
          break;
        default:
          break;
      }
    }
  }
  // A list's (or D3D's buffer's) draws are classed by the buffer, not by marks.
  if (state.lean && (level.context == census::kContextList ||
                     level.context == census::kContextD3dBuffer)) {
    return;
  }
  const uint8_t source =
      census::FindPacket(level.ptr + offset, level.context == census::kContextList);
  if (source == census::kSourceNone || source == level.source) return;
  ++state.window.marks_found;
  SetSource(level, source);
  SwitchTime(TimeClassOf(level));
}

bool Visible() { return !state.stack.empty() && state.stack.back().visible; }

void Draw(bool drawn, bool copy) {
  if (copy && !state.lean) {
    if (census::ListDecoded* list = InnermostList()) ++list->copies;
  }
  if (!drawn) return;
  if (state.stack.empty()) {
    if (census::Enabled()) {
      ++state.window.drawn[census::kDrawUnknownBuffer];
      PERF_counter_inc(kD3dCensusFallbackDraws);
    }
    return;
  }
  const Level& level = state.stack.back();
  if (level.visible) {
    PERF_counter_inc(kD3dCensusVisibleDraws);
  } else {
    PERF_counter_inc(kD3dCensusFallbackDraws);
  }
  const uint8_t draw_class = DrawClassOf(level);
  ++state.window.drawn[draw_class];
  ++state.window.drawn_by_source[level.source][level.context];
  if (level.context == census::kContextList) {
    ++state.window.list_drawn_by_state[level.state < census::kListStateCount ? level.state : 0];
    ++state.window.list_drawn_by_origin[level.origin & 31u];
  }
  if (!state.lean) {
    if (census::ListDecoded* list = InnermostList()) ++list->draws;
  }
  if (state.span_open) ++state.span_draws;
}

void Swap() {
  if (!census::Enabled() && state.stack.empty()) {
    if (state.window.swaps) {
      census::AddDecoderWindow(state.window);
      state.window = {};
    }
    return;
  }
  if (state.time_class != kNoTimeClass) SwitchTime(state.time_class);
  for (const auto& [object, counts] : state.lists) census::AddListDecoded(object, counts);
  state.lists.clear();
  ++state.window.swaps;
  census::AddDecoderWindow(state.window);
  state.window = {};
}

}  // namespace rex::graphics::d3d_census_decoder
