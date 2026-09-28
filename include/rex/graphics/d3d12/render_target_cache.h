/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/d3d12/d3d12_cpu_descriptor_pool.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>
#include <rex/ui/d3d12/d3d12_util.h>
namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

class D3D12RenderTargetCache final : public RenderTargetCache {
 public:
  D3D12RenderTargetCache(const RegisterFile& register_file, const memory::Memory& memory,
                         uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                         D3D12CommandProcessor& command_processor, bool bindless_resources_used)
      : RenderTargetCache(register_file, memory, draw_resolution_scale_x, draw_resolution_scale_y),
        command_processor_(command_processor),
        bindless_resources_used_(bindless_resources_used) {}
  ~D3D12RenderTargetCache() override;

  // The FH1 native executor owns EDRAM, render targets, transfers and
  // resolves. Only the host configuration the pipelines are built against is
  // derived; the cache must not be updated or asked to resolve.
  bool Initialize();
  void Shutdown(bool from_destructor = false);

  Path GetPath() const override { return Path::kHostRenderTargets; }

  bool msaa_2x_supported() const { return msaa_2x_supported_; }

  void WriteEdramUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                       uint32_t element_size_bytes_pow2);

  // For host render targets.

  bool gamma_render_target_as_unorm16() const { return gamma_render_target_as_unorm16_; }

  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  // Persistent data doesn't depend on this, so can be overriden by per-game
  // configuration.
  bool IsFixed16TruncatedToMinus1To1() const {
    return GetPath() == Path::kHostRenderTargets && !REXCVAR_GET(snorm16_render_target_full_range);
  }

  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  DXGI_FORMAT GetColorResourceDXGIFormat(xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT GetColorDrawDXGIFormat(xenos::ColorRenderTargetFormat format) const;
  static DXGI_FORMAT GetDepthResourceDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVDepthDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVStencilDXGIFormat(xenos::DepthRenderTargetFormat format);

 protected:
  uint32_t GetMaxRenderTargetWidth() const override { return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; }
  uint32_t GetMaxRenderTargetHeight() const override {
    return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  }

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;

  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

  bool IsGammaFormatHostStorageSeparate() const override;

 private:
  D3D12CommandProcessor& command_processor_;
  [[maybe_unused]] bool bindless_resources_used_;

  // Indices of the EDRAM buffer views in the non-shader-visible descriptor
  // heap used for binding by copying.
  enum class EdramBufferDescriptorIndex : uint32_t {
    kRawSRV,
    kR32UintSRV,
    kR32G32UintSRV,
    kR32G32B32A32UintSRV,
    kRawUAV,
    kR32UintUAV,
    kR32G32UintUAV,
    kR32G32B32A32UintUAV,

    kCount,
  };
  D3D12_CPU_DESCRIPTOR_HANDLE edram_buffer_descriptor_heap_start_{};

  // Host render target configuration shared with the pipeline cache.
  void InitializeHostConfig();

  bool use_stencil_reference_output_ = false;

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_supported_ = false;
};

}  // namespace rex::graphics::d3d12
