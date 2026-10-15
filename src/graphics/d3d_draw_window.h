#pragma once

// DL-1.2 D3D-level draws: the command processor's window state (see
// CommandProcessor::D3dDrawMarker and rex/system/d3d_draw.h). Private to
// command_processor.cpp and d3d_draw_path.cpp.

#include <cstdint>
#include <map>

#include <rex/graphics/command_processor.h>
#include <rex/system/d3d_draw.h>

namespace rex::graphics {

struct CommandProcessor::D3dDrawWindow {
  // Registers a window can cover: the register file (with the constant
  // flush registers at 0x5000-0x5002), rounded up to 64.
  static constexpr uint32_t kCoveredRegisters = 0x5040;
  // A window that has not reached its draw packet after this many packets
  // is abandoned.
  static constexpr uint32_t kMaxPackets = 4096;

  system::d3d_draw::Description description;
  uint32_t mode = 0;
  // DL-2.1: a replacement marker's verify window. The call is replaced in
  // draw mode, so every packet of the window that the description does not
  // cover is counted as uncovered.
  bool slice = false;
  uint32_t uncovered = 0;
  // The first undescribed packets' keys (as uncovered_packets), counted at
  // the draw unless the window was carried across segments: a segment kick
  // inside the call writes its own packets (the replaced call needs no kick).
  uint32_t uncovered_keys[16] = {};
  uint32_t depth = 0;
  uint32_t packets = 0;
  uint32_t segments = 0;
  // Shader types the description loads (vertex, pixel).
  bool has_shader[2] = {};
  uint64_t covered[kCoveredRegisters / 64] = {};
  // Verify: the loads the packets made, by type.
  struct SeenLoad {
    uint32_t count = 0;
    uint32_t address = 0;
    uint32_t dwords = 0;
    uint64_t hash = 0;
  } seen[2];

  bool Covered(uint32_t index) const {
    return index < kCoveredRegisters && ((covered[index >> 6] >> (index & 63)) & 1);
  }
  bool CoveredRange(uint32_t first, uint32_t count) const {
    if (!count) return false;
    for (uint32_t i = 0; i < count; ++i) {
      if (!Covered(first + i)) return false;
    }
    return true;
  }

  enum Abort : uint32_t {
    kAbortNestedIb,
    kAbortMarker,
    kAbortSwap,
    kAbortLimit,
    kAbortPredicated,
    kAbortCount,
  };
  struct Stats {
    uint64_t markers = 0;
    uint64_t fetch_failed = 0;
    uint64_t declined = 0;
    uint64_t opened = 0;
    uint64_t carried = 0;
    uint64_t aborted[kAbortCount] = {};
    // Verify.
    uint64_t verified = 0;
    uint64_t verified_registers = 0;
    uint64_t register_mismatch_draws = 0;
    uint64_t register_mismatches = 0;
    uint64_t shader_mismatch_draws = 0;
    uint64_t draw_mismatch_draws = 0;
    // Draw.
    uint64_t drawn = 0;
    uint64_t skipped_packets = 0;
    uint64_t kept_packets = 0;
    uint64_t swaps = 0;
    // DL-2.1 slice.
    uint64_t slice_markers = 0;
    uint64_t slice_lost = 0;
    uint64_t slice_drawn = 0;
    uint64_t slice_predicated_out = 0;
    uint64_t slice_windows = 0;
    uint64_t slice_verified = 0;
    uint64_t slice_uncovered_draws = 0;
    uint64_t slice_uncovered_packets = 0;
    uint64_t slice_auto_index = 0;
    uint64_t slice_carried_uncovered = 0;
  } stats;
  std::map<uint32_t, uint64_t> mismatch_registers;
  // DL-2.1 verify: undescribed packets by (type-0 first register) or
  // (0x10000 | type-3 opcode).
  std::map<uint32_t, uint64_t> uncovered_packets;
  uint32_t mismatch_logs = 0;
};

}  // namespace rex::graphics
