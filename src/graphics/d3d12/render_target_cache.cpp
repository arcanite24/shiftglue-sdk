/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(native_stencil_value_output_d3d12_intel, false, "GPU/D3D12",
                    "Native stencil value output for Intel D3D12");

REXCVAR_DEFINE_BOOL(native_stencil_value_output, true, "GPU", "Enable native stencil value output");

namespace rex::graphics::d3d12 {

D3D12RenderTargetCache::~D3D12RenderTargetCache() {
  Shutdown(true);
}

void D3D12RenderTargetCache::InitializeHostConfig() {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Using the cvar on emulator initialization so used pipelines are consistent
  // across different titles launched in one emulator instance.
  use_stencil_reference_output_ =
      REXCVAR_GET(native_stencil_value_output) &&
      provider.IsPSSpecifiedStencilReferenceSupported() &&
      (REXCVAR_GET(native_stencil_value_output_d3d12_intel) ||
       provider.GetAdapterVendorID() != ui::GraphicsProvider::GpuVendorID::kIntel);

  if (GetPath() != Path::kHostRenderTargets) return;

  gamma_render_target_as_unorm16_ = REXCVAR_GET(gamma_render_target_as_unorm16);

  depth_float24_round_ = REXCVAR_GET(depth_float24_round);
  depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);

  // Check if 2x MSAA is supported or needs to be emulated with 4x MSAA
  // instead.
  if (REXCVAR_GET(native_2x_msaa)) {
    msaa_2x_supported_ = true;
    static const DXGI_FORMAT kRenderTargetDXGIFormats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_SNORM,
        DXGI_FORMAT_R32G32_FLOAT,
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R16G16_SNORM,
        DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        // For ownership transfer.
        DXGI_FORMAT_R16G16B16A16_UINT,
        DXGI_FORMAT_R32G32_UINT,
        DXGI_FORMAT_R16G16_UINT,
        DXGI_FORMAT_R32_UINT,
    };
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
    multisample_quality_levels.SampleCount = 2;
    multisample_quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    for (size_t i = 0; i < rex::countof(kRenderTargetDXGIFormats); ++i) {
      multisample_quality_levels.Format = kRenderTargetDXGIFormats[i];
      multisample_quality_levels.NumQualityLevels = 0;
      if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                             &multisample_quality_levels,
                                             sizeof(multisample_quality_levels))) ||
          !multisample_quality_levels.NumQualityLevels) {
        msaa_2x_supported_ = false;
        break;
      }
    }
  } else {
    msaa_2x_supported_ = false;
  }
  if (msaa_2x_supported_ && gamma_render_target_as_unorm16_) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
    multisample_quality_levels.SampleCount = 2;
    multisample_quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    multisample_quality_levels.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    multisample_quality_levels.NumQualityLevels = 0;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                           &multisample_quality_levels,
                                           sizeof(multisample_quality_levels))) ||
        !multisample_quality_levels.NumQualityLevels) {
      msaa_2x_supported_ = false;
    }
  }
  if (!msaa_2x_supported_) {
    REXGPU_WARN(
        "2x MSAA is not supported, emulated via top-left and bottom-right "
        "samples of 4x MSAA");
  }
}

bool D3D12RenderTargetCache::Initialize() {
  // The FH1 native executor owns EDRAM, render targets, transfers and
  // resolves; only the host configuration the pipelines are built against is
  // derived here.
  InitializeHostConfig();
  return true;
}

void D3D12RenderTargetCache::Shutdown(bool from_destructor) {
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12RenderTargetCache::WriteEdramUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                             uint32_t element_size_bytes_pow2) {
  EdramBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EdramBufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(1, handle,
                                provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                                              uint32_t(descriptor_index)),
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorResourceDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  // Typed should be preferred over typeless so there are more opportunities for
  // compression.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? DXGI_FORMAT_R16G16B16A16_UNORM
                                             : DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    // SNORM has two representations of -1.
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // Floating-point - ensure NaN propagation during ownership transfer for
    // unmodified data.
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // TODO(Triang3l): Check if NaN propagation defined in the D3D11.3
    // specification can be relied on for 32-bit float render targets.
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorDrawDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return GetColorResourceDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthResourceDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24G8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVDepthDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVStencilDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

RenderTargetCache::RenderTarget* D3D12RenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  // Host render targets are owned by the FH1 native executor.
  (void)key;
  return nullptr;
}

bool D3D12RenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24FS8) {
    return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

bool D3D12RenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

}  // namespace rex::graphics::d3d12
