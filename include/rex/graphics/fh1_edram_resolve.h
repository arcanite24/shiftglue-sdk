#pragma once
// Resolve (EDRAM to memory copy) planning for the FH1 native executor
// (NP-9.0): reads the guest's resolve registers into what to copy, from which
// EDRAM surface, into which memory format. API-agnostic; the backend runs the
// copy for each owner of the source tiles.

#include <cstdint>
#include <string>

#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

namespace rex::memory {
class Memory;
}

namespace rex::graphics {

class RegisterFile;

// Backend conventions the plan encodes into its destination word.
struct Fh1ResolveFlags {
  bool depth_float24_round = false;
  bool gamma_as_unorm16 = false;
  bool fixed16_truncated = false;
};

struct Fh1ResolvePlan {
  bool empty = false;         // Nothing to copy or clear.
  const char* skip = nullptr;  // Why the copy cannot run natively.
  bool copy = false;           // The copy can run natively.
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // Source pixels.
  draw_util::ResolveInfo info;
  uint32_t msaa = 0;
  uint32_t pitch_tiles = 0;
  bool copying_depth = false;
  reg::RB_COLOR_INFO color_info;
  reg::RB_DEPTH_INFO depth_info;
  // The source surface, with the format the resolve names for colors.
  Fh1SurfaceKey resolve_key;
  // Packed destination format, endianness, swap, exponent bias and flags.
  uint32_t dest_info = 0;
  uint32_t dest_base = 0;
  uint32_t dest_pitch = 0;
  uint32_t sample_select = 0;
  std::string kind;  // For statistics.
};

// Same rectangle derivation as draw_util::GetResolveInfo, in surface pixels
// relative to the base in the color or depth info register.
bool Fh1ResolveRectangle(const RegisterFile& regs, const memory::Memory& memory, int32_t& x0,
                         int32_t& y0, int32_t& x1, int32_t& y1);

// Color formats whose guest EDRAM words the resolve shader can encode and
// decode.
bool Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat format);

// False when there is nothing to do (plan.empty) or the resolve information
// is unusable (plan.skip); true otherwise, with plan.copy set when the copy
// can run natively and plan.skip naming why not.
bool Fh1PlanResolve(const RegisterFile& regs, const memory::Memory& memory,
                    const Fh1ResolveFlags& flags, Fh1ResolvePlan& plan);

}  // namespace rex::graphics
