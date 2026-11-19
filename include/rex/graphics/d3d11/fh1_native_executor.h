/**
 * @file        graphics/d3d11/fh1_native_executor.h
 * @brief       Direct3D 11 FH1 native executor (render surfaces, EDRAM tile
 *              ownership, transfers, clears and resolves).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>

#include <rex/graphics/fh1_depth_overwrite.h>
#include <rex/graphics/fh1_edram_resolve.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::memory {
class Memory;
}

namespace rex::graphics {
class RegisterFile;
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;
class D3D11SharedMemory;
class D3D11TextureCache;

// State of a consumed guest draw that the executor needs, captured by
// D3D11CommandProcessor::IssueDraw.
using Fh1NativeDrawInfo = rex::graphics::Fh1DrawInfo;

struct Fh1NativeExecutorConfig {
  bool msaa_2x_supported = true;
  bool gamma_as_unorm16 = false;
  bool depth_float24_round = false;
  // 16_16[_16_16] host targets are snorm limited to -1...1.
  bool fixed16_truncated = true;
  D3D11SharedMemory* memory = nullptr;
  D3D11TextureCache* textures = nullptr;
};

// The Direct3D 11 port of the Direct3D 12 FH1 native executor
// (d3d12/fh1_native_executor.h), with the same behavior, unscaled only
// (resolution scale 1): every consumed guest draw is drawn into native render
// surfaces whose EDRAM tile ownership, clears and resolves the executor tracks
// from the guest registers. Resolves write the guest texture layout into the
// shared memory mirror (through its UAVs), so later fetches of resolve output
// decode native data like any other guest texture.
//
// Threading and state: GPU commands thread only, on the immediate context.
// Its own transfer, clear and resolve passes change context state, so it
// calls D3D11CommandProcessor::InvalidateGuestDrawState() after them, and
// unbinds the UAVs it bound. BindTargets binds the render target and depth
// stencil views for the guest draw with OMSetRenderTargets.
class Fh1NativeExecutor {
 public:
  Fh1NativeExecutor(D3D11CommandProcessor& command_processor, const RegisterFile& register_file,
                    memory::Memory& memory);
  ~Fh1NativeExecutor();

  bool Initialize(const Fh1NativeExecutorConfig& config);
  void Shutdown();

  // Before the targets are bound for a draw: derives the surfaces the draw
  // writes and takes their EDRAM tiles, transferring the previous owners'
  // contents like the guest's EDRAM aliasing does.
  void PrepareTargets(const Fh1NativeDrawInfo& draw);
  void LogStats(uint64_t frame);
  // Frame dumps are not supported on Direct3D 11 (replays are).
  void RecordCopyInputs() {}
  void RecordDrawInputs(uint32_t /*used_texture_mask*/, const Shader& /*vertex_shader*/,
                        uint32_t /*guest_dma_index_offset*/, uint32_t /*guest_dma_index_size*/) {}

  // Binds the surfaces PrepareTargets derived and returns the bound slots
  // (bit 0 depth, bits 1-4 color) and their formats (index 0 depth as
  // xenos::DepthRenderTargetFormat, 1-4 color as
  // xenos::ColorRenderTargetFormat). False skips the draw (the reason is
  // counted). bound_width and bound_height are the smallest size, in host
  // pixels, of the bound surfaces (0 when none is bound).
  bool BindTargets(uint32_t& bound_bits, uint32_t* formats, uint32_t& bound_width,
                   uint32_t& bound_height);
  // After the command processor recorded the draw.
  void NativeDrawIssued(const Fh1NativeDrawInfo& draw);
  // In place of the render target cache's resolve. Returns whether the copy
  // (or clear) ran, and the guest range it wrote.
  bool NativeResolve(uint32_t& written_address, uint32_t& written_length);
  // Before the guest CPU can observe GPU progress: copies the one-off resolve
  // read-backs recorded since into guest memory, waiting for the GPU once.
  void FlushResolveReadbacks();
  // End of a guest frame, at the swap: periodic stats and native front-buffer
  // PPM dumps.
  void OnSwap(uint64_t frame, uint32_t frontbuffer_address, uint32_t width, uint32_t height,
              const uint32_t* gamma_pwl);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::graphics::d3d11
