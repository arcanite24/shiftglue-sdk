#pragma once
// Depth-overwriting draws for the FH1 native executor (NP-9.0): a draw that
// writes depth with an ALWAYS test over a few screen-aligned rectangles
// replaces the depth of every sample it covers, so the tiles it covers need
// no transfer from their previous owner. API-agnostic: runs the vertex shader
// on the CPU and returns the rectangles.

#include <array>
#include <cstdint>
#include <vector>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/registers.h>

namespace rex::memory {
class Memory;
}

namespace rex::graphics {

class RegisterFile;
class Shader;

// What the command processor knows about a draw when the native executor
// prepares and records it.
struct Fh1DrawInfo {
  bool memexport = false;
  bool occlusion_query_active = false;
  bool rasterization_done = false;
  reg::RB_DEPTHCONTROL normalized_depth_control;
  uint32_t normalized_color_mask = 0;
  const Shader* vertex_shader = nullptr;
  const Shader* pixel_shader = nullptr;
};

class Fh1DepthOverwrite {
 public:
  // A depth-only rectangle in guest pixels [x0, y0, x1, y1): `inner` has the
  // depth of every sample overwritten, `outer` bounds every pixel the draw
  // can touch.
  struct Rect {
    std::array<int32_t, 4> inner;
    std::array<int32_t, 4> outer;
  };

  Fh1DepthOverwrite(const RegisterFile& register_file, const memory::Memory& memory)
      : register_file_(register_file), interpreter_(register_file, memory) {}

  // Finds the draw's rectangles into rects(). False when the draw is not a
  // depth overwrite the interpreter can follow. `stencil_overwritten`: whether
  // the draw also rewrites all stencil bits with one reference value.
  bool Derive(const Fh1DrawInfo& draw, bool half_pixel_offset, bool& stencil_overwritten);
  const std::vector<Rect>& rects() const { return rects_; }

 private:
  class PositionSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;
    std::array<float, 4> position{};
    uint32_t position_mask = 0;
    bool killed = false;
  };

  const RegisterFile& register_file_;
  ShaderInterpreter interpreter_;
  std::vector<Rect> rects_;
};

}  // namespace rex::graphics
