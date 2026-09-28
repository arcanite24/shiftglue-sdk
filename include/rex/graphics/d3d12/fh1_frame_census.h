#pragma once

#include <cstdint>

namespace rex::graphics {
class RegisterFile;
}

namespace rex::graphics::d3d12 {

// Metadata-only census of the consumed guest frame: which render surfaces,
// draw states, texture formats, resolves and swaps each window of frames
// uses. It copies no payloads and aggregates into bounded tables that are
// flushed as one JSON line per window to `fh1_frame_census_path`. The native
// renderer's frame contract is derived from these records.
struct Fh1CensusDraw {
  uint64_t frame = 0;
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;
  uint64_t vertex_modification = 0;
  uint64_t pixel_modification = 0;
  uint64_t pipeline_hash = 0;
  uint32_t guest_primitive = 0;
  uint32_t host_primitive = 0;
  uint32_t index_format = 0;
  uint32_t index_count = 0;
  bool indexed = false;
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t bound_render_target_bits = 0;
  uint32_t host_render_target_formats[5] = {};
  uint32_t used_texture_mask = 0;
  bool memexport = false;
  bool occlusion_query = false;
};

class Fh1FrameCensus {
 public:
  static bool Enabled();
  static void ObserveDraw(const RegisterFile& regs, const Fh1CensusDraw& draw);
  static void ObserveOptimizedClear(uint64_t frame, uint32_t mode);
  static void ObserveCopy(const RegisterFile& regs, uint64_t frame,
                          uint32_t written_address, uint32_t written_length,
                          bool succeeded);
  static void ObserveZpd(uint64_t frame, uint32_t sample_count_address);
  static void ObserveSwap(uint64_t frame, uint32_t frontbuffer_address,
                          uint32_t width, uint32_t height, uint32_t format);
};

}  // namespace rex::graphics::d3d12
