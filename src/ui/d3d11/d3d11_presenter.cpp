/**
 ******************************************************************************
 * ShiftGlue: Direct3D 11 UI backend                                          *
 ******************************************************************************
 * Based on the Xenia Direct3D 12 presenter, Copyright 2022 Ben Vanik.        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d11/d3d11_presenter.h>
#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/surface_win.h>

REXCVAR_DEFINE_BOOL(d3d11_allow_variable_refresh_rate_and_tearing, true, "UI/D3D11",
                    "Allow variable refresh rate and tearing (flip model swap chains on "
                    "DXGI with tearing support, while vsync is off)");

namespace rex::ui::d3d11 {

// Generated with src/ui/shaders/compile-d3d11.sh.
namespace shaders {
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_bilinear_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_bilinear_ps.h"
#if defined(REX_HAS_FIDELITYFX_SDK)
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_cas_resample_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_cas_resample_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_cas_sharpen_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_cas_sharpen_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_fsr_easu_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_fsr_rcas_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_fsr_rcas_ps.h"
#endif
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

namespace {

// The largest effect constants, rounded up to the 16-byte cbuffer granularity.
// (Checked against each constants struct in PaintAndPresentImpl.)
constexpr UINT kGuestOutputPaintEffectConstantsSize = 32;

// vsync belongs to the command processor (in the GPU plugin, so it is read by
// name, as in the Vulkan presenter). Unregistered counts as on.
bool IsVsyncRequested() { return rex::cvar::GetFlagByName("vsync") != "false"; }

}  // namespace

D3D11Presenter::~D3D11Presenter() {
  // Nothing to await: the D3D11 runtime keeps resources referenced by
  // submitted work alive. Only the swap chain needs its buffers released.
  paint_context_.DestroySwapChain(provider_.GetImmediateContext());
}

Surface::TypeFlags D3D11Presenter::GetSupportedSurfaceTypes() const {
  Surface::TypeFlags types = 0;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
  types |= Surface::kTypeFlag_Win32Hwnd;
#endif
  return types;
}

bool D3D11Presenter::CaptureGuestOutput(RawImage& image_out) {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> guest_output_texture;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(guest_output_mailbox_index, nullptr, nullptr));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_texture = guest_output_textures_[guest_output_mailbox_index].texture;
    }
    // Incremented the reference count of the guest output texture - safe to
    // leave the consumer critical section now.
  }
  if (!guest_output_texture) {
    return false;
  }

  ID3D11Device* device = provider_.GetDevice();

  D3D11_TEXTURE2D_DESC staging_desc;
  guest_output_texture->GetDesc(&staging_desc);
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output capture texture");
    return false;
  }

  const uint32_t width = staging_desc.Width;
  const uint32_t height = staging_desc.Height;
  std::vector<uint32_t> packed(size_t(width) * height);
  {
    // CopyResource and Map bind no pipeline state, so they can run on the
    // immediate context between the command processor's calls; the lock keeps
    // the pair together.
    D3D11Provider::ContextLock context_lock = provider_.LockImmediateContext();
    ID3D11DeviceContext* context = provider_.GetImmediateContext();
    context->CopyResource(staging.Get(), guest_output_texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      REXLOG_ERROR("D3D11Presenter: Failed to map the guest output capture texture");
      return false;
    }
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(&packed[size_t(width) * y],
                  static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y,
                  sizeof(uint32_t) * width);
    }
    context->Unmap(staging.Get(), 0);
  }

  image_out.width = width;
  image_out.height = height;
  image_out.stride = sizeof(uint32_t) * width;
  image_out.data.resize(image_out.stride * height);
  uint32_t* image_out_pixels = reinterpret_cast<uint32_t*>(image_out.data.data());
  for (size_t i = 0; i < packed.size(); ++i) {
    image_out_pixels[i] = Packed10bpcRGBTo8bpcBytes(packed[i]);
  }
  return true;
}

bool D3D11Presenter::CapturePresentedOutput(RawImage& image_out) {
  if (!presented_capture_enabled_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(presented_capture_mutex_);
  if (!presented_capture_written_ || !presented_capture_texture_) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  presented_capture_texture_->GetDesc(&desc);
  RawImage image;
  image.width = desc.Width;
  image.height = desc.Height;
  image.stride = size_t(desc.Width) * 4;
  image.data.resize(image.stride * desc.Height);
  {
    D3D11Provider::ContextLock context_lock = provider_.LockImmediateContext();
    ID3D11DeviceContext* context = provider_.GetImmediateContext();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(presented_capture_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return false;
    }
    for (uint32_t y = 0; y < desc.Height; ++y) {
      std::memcpy(image.data.data() + image.stride * y,
                  static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y,
                  image.stride);
    }
    context->Unmap(presented_capture_texture_.Get(), 0);
  }
  // B8G8R8A8 to R8G8B8X8.
  for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
    std::swap(image.data[i], image.data[i + 2]);
    image.data[i + 3] = 0xFF;
  }
  image_out = std::move(image);
  return true;
}

Presenter::PresentStats D3D11Presenter::GetPresentStatsFromUIThread() const {
  PresentStats stats;
  stats.real_presents = stats_real_presents_.load(std::memory_order_relaxed);
  stats.render_width = stats_render_width_.load(std::memory_order_relaxed);
  stats.render_height = stats_render_height_.load(std::memory_order_relaxed);
  stats.output_width = stats_output_width_.load(std::memory_order_relaxed);
  stats.output_height = stats_output_height_.load(std::memory_order_relaxed);
  stats.hdr = false;
  // Presents never wait for the vertical blank (sync interval 0, like the
  // Direct3D 12 presenter); with vsync on they just don't tear, like
  // Vulkan's mailbox mode, which reports false too.
  stats.vsync = false;
  return stats;
}

Presenter::SurfacePaintConnectResult
D3D11Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface& new_surface,
                                                                uint32_t new_surface_width,
                                                                uint32_t new_surface_height,
                                                                bool was_paintable,
                                                                bool& is_vsync_implicit_out) {
  uint32_t new_swap_chain_width =
      std::min(new_surface_width, uint32_t(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));
  uint32_t new_swap_chain_height =
      std::min(new_surface_height, uint32_t(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));

  // ResizeBuffers cannot toggle tearing, so a changed preference recreates the
  // swap chain (a surface resize is how callers request that).
  const bool allow_tearing =
      REXCVAR_GET(d3d11_allow_variable_refresh_rate_and_tearing) && dxgi_supports_tearing_;
  if (paint_context_.swap_chain &&
      paint_context_.swap_effect != SwapEffect::kDiscard &&
      paint_context_.swap_chain_allows_tearing != allow_tearing) {
    paint_context_.DestroySwapChain(provider_.GetImmediateContext());
  }
  if (paint_context_.swap_chain) {
    if (was_paintable && paint_context_.swap_chain_width == new_swap_chain_width &&
        paint_context_.swap_chain_height == new_swap_chain_height) {
      is_vsync_implicit_out = false;
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    // All references to the buffers must be released before ResizeBuffers. The
    // deferred context is reset after each FinishCommandList(FALSE), and
    // executed command lists are released right away, so only the presenter's
    // own references remain.
    paint_context_.back_buffer_rtv.Reset();
    paint_context_.back_buffer.Reset();
    {
      std::lock_guard<std::mutex> capture_lock(presented_capture_mutex_);
      presented_capture_texture_.Reset();
      presented_capture_written_ = false;
    }
    // Let the deferred destruction of the released views run first.
    provider_.GetImmediateContext()->Flush();
    bool swap_chain_resized = SUCCEEDED(paint_context_.swap_chain->ResizeBuffers(
        0, UINT(new_swap_chain_width), UINT(new_swap_chain_height), DXGI_FORMAT_UNKNOWN,
        paint_context_.swap_chain_flags));
    if (swap_chain_resized) {
      swap_chain_resized = SUCCEEDED(
          paint_context_.swap_chain->GetBuffer(0, IID_PPV_ARGS(&paint_context_.back_buffer)));
    }
    if (swap_chain_resized) {
      paint_context_.swap_chain_width = new_swap_chain_width;
      paint_context_.swap_chain_height = new_swap_chain_height;
    } else {
      REXLOG_ERROR("D3D11Presenter: Failed to resize a swap chain");
      // Failed to resize, retry creating from scratch.
      paint_context_.DestroySwapChain(provider_.GetImmediateContext());
    }
  }

  if (!paint_context_.swap_chain) {
    Surface::TypeIndex surface_type = new_surface.GetType();
    HWND surface_hwnd = nullptr;
    switch (surface_type) {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
      case Surface::kTypeIndex_Win32Hwnd:
        surface_hwnd = static_cast<const Win32HwndSurface&>(new_surface).hwnd();
        break;
#endif
      default:
        assert_unhandled_case(surface_type);
        REXLOG_ERROR(
            "D3D11Presenter: Tried to create a swap chain for an unsupported surface type");
        return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }

    ID3D11Device* device = provider_.GetDevice();
    IDXGIFactory2* dxgi_factory2 = provider_.GetDXGIFactory2();
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain;
    SwapEffect swap_effect = SwapEffect::kDiscard;
    UINT swap_chain_flags = 0;
    if (dxgi_factory2) {
      // Flip model: FLIP_DISCARD (Windows 10), then FLIP_SEQUENTIAL (Windows
      // 8), then the blt model DISCARD.
      const SwapEffect attempts[] = {SwapEffect::kFlipDiscard, SwapEffect::kFlipSequential,
                                     SwapEffect::kDiscard};
      for (SwapEffect attempt : attempts) {
        bool is_flip = attempt != SwapEffect::kDiscard;
        DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
        swap_chain_desc.Width = UINT(new_swap_chain_width);
        swap_chain_desc.Height = UINT(new_swap_chain_height);
        swap_chain_desc.Format = kSwapChainFormat;
        swap_chain_desc.Stereo = FALSE;
        swap_chain_desc.SampleDesc.Count = 1;
        swap_chain_desc.SampleDesc.Quality = 0;
        swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc.BufferCount = is_flip ? PaintContext::kFlipSwapChainBufferCount
                                              : PaintContext::kBltSwapChainBufferCount;
        // DXGI_SCALING_STRETCH may cause the content to "shake" while resizing
        // (see the Direct3D 12 presenter). DXGI_SCALING_NONE is flip-only.
        swap_chain_desc.Scaling =
            (is_flip && new_swap_chain_width == new_surface_width &&
             new_swap_chain_height == new_surface_height)
                ? DXGI_SCALING_NONE
                : DXGI_SCALING_STRETCH;
        switch (attempt) {
          case SwapEffect::kFlipDiscard:
            swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            break;
          case SwapEffect::kFlipSequential:
            swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            break;
          default:
            swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            break;
        }
        swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swap_chain_desc.Flags = 0;
        if (is_flip && allow_tearing) {
          // Allow tearing in borderless fullscreen to support variable
          // refresh rate.
          swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_1;
        HRESULT create_result = dxgi_factory2->CreateSwapChainForHwnd(
            device, surface_hwnd, &swap_chain_desc, nullptr, nullptr, &swap_chain_1);
        if (SUCCEEDED(create_result)) {
          swap_chain = swap_chain_1;
          swap_effect = attempt;
          swap_chain_flags = swap_chain_desc.Flags;
          break;
        }
        REXLOG_WARN(
            "D3D11Presenter: Failed to create a swap chain with swap effect {} (HRESULT "
            "0x{:08X}), trying the next one",
            uint32_t(swap_chain_desc.SwapEffect), static_cast<unsigned>(create_result));
      }
    } else {
      // DXGI 1.1 (Windows 7 without the platform update): blt model only.
      DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
      swap_chain_desc.BufferDesc.Width = UINT(new_swap_chain_width);
      swap_chain_desc.BufferDesc.Height = UINT(new_swap_chain_height);
      swap_chain_desc.BufferDesc.Format = kSwapChainFormat;
      swap_chain_desc.SampleDesc.Count = 1;
      swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      swap_chain_desc.BufferCount = PaintContext::kBltSwapChainBufferCount;
      swap_chain_desc.OutputWindow = surface_hwnd;
      swap_chain_desc.Windowed = TRUE;
      swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      if (FAILED(provider_.GetDXGIFactory()->CreateSwapChain(device, &swap_chain_desc,
                                                             &swap_chain))) {
        swap_chain.Reset();
      }
    }
    if (!swap_chain) {
      REXLOG_ERROR("D3D11Presenter: Failed to create a swap chain for the HWND");
      return SurfacePaintConnectResult::kFailure;
    }
    // Disable automatic Alt+Enter handling - DXGI fullscreen doesn't support
    // ALLOW_TEARING, and the window implementation provides borderless
    // fullscreen anyway with better state tracking.
    provider_.GetDXGIFactory()->MakeWindowAssociation(surface_hwnd, DXGI_MWA_NO_ALT_ENTER);

    paint_context_.swap_chain = swap_chain;
    paint_context_.swap_effect = swap_effect;
    paint_context_.swap_chain_flags = swap_chain_flags;
    paint_context_.swap_chain_allows_tearing =
        (swap_chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    paint_context_.swap_chain_width = new_swap_chain_width;
    paint_context_.swap_chain_height = new_swap_chain_height;
    // From now on, in case of any failure, DestroySwapChain must be called
    // before returning.
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&paint_context_.back_buffer)))) {
      REXLOG_ERROR("D3D11Presenter: Failed to get the swap chain back buffer");
      paint_context_.DestroySwapChain(provider_.GetImmediateContext());
      return SurfacePaintConnectResult::kFailure;
    }
    static const char* const kSwapEffectNames[] = {"FLIP_DISCARD", "FLIP_SEQUENTIAL",
                                                   "DISCARD (blt)"};
    REXLOG_INFO("D3D11Presenter: {}x{} {} swap chain{}", new_swap_chain_width,
                new_swap_chain_height, kSwapEffectNames[size_t(swap_effect)],
                paint_context_.swap_chain_allows_tearing ? " with tearing" : "");
  }

  // The back buffer RTV.
  D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
  rtv_desc.Format = kSwapChainFormat;
  rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  rtv_desc.Texture2D.MipSlice = 0;
  if (FAILED(provider_.GetDevice()->CreateRenderTargetView(
          paint_context_.back_buffer.Get(), &rtv_desc,
          paint_context_.back_buffer_rtv.ReleaseAndGetAddressOf()))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the swap chain back buffer RTV");
    paint_context_.DestroySwapChain(provider_.GetImmediateContext());
    return SurfacePaintConnectResult::kFailure;
  }

  is_vsync_implicit_out = false;
  return SurfacePaintConnectResult::kSuccess;
}

void D3D11Presenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  paint_context_.DestroySwapChain(provider_.GetImmediateContext());
  std::lock_guard<std::mutex> capture_lock(presented_capture_mutex_);
  presented_capture_texture_.Reset();
  presented_capture_written_ = false;
}

void D3D11Presenter::PaintContext::DestroySwapChain(ID3D11DeviceContext* immediate_context) {
  if (!swap_chain) {
    return;
  }
  back_buffer_rtv.Reset();
  back_buffer.Reset();
  swap_chain.Reset();
  swap_chain_flags = 0;
  swap_chain_allows_tearing = false;
  swap_chain_height = 0;
  swap_chain_width = 0;
  // Flush binds no state, so it's allowed on the shared immediate context.
  if (immediate_context) {
    immediate_context->Flush();
  }
}

bool D3D11Presenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  assert_not_zero(frontbuffer_width);
  assert_not_zero(frontbuffer_height);
  GuestOutputTexture& guest_output = guest_output_textures_[mailbox_index];
  if (guest_output.texture &&
      (guest_output.width != frontbuffer_width || guest_output.height != frontbuffer_height)) {
    // Painting holds its own references while it uses the texture, and the
    // runtime keeps it alive for submitted work - safe to drop here.
    guest_output = GuestOutputTexture();
  }
  if (!guest_output.texture) {
    ID3D11Device* device = provider_.GetDevice();
    const bool uav_supported = provider_.IsGuestOutputFormatUAVStoreSupported();
    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = frontbuffer_width;
    texture_desc.Height = frontbuffer_height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = kGuestOutputFormat;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags =
        kGuestOutputBindFlags | (uav_supported ? D3D11_BIND_UNORDERED_ACCESS : 0);
    GuestOutputTexture new_guest_output;
    if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &new_guest_output.texture))) {
      REXLOG_ERROR("D3D11Presenter: Failed to create the guest output {}x{} texture",
                   frontbuffer_width, frontbuffer_height);
      return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = kGuestOutputFormat;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = kGuestOutputFormat;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;
    if (FAILED(device->CreateShaderResourceView(new_guest_output.texture.Get(), &srv_desc,
                                                &new_guest_output.srv)) ||
        FAILED(device->CreateRenderTargetView(new_guest_output.texture.Get(), &rtv_desc,
                                              &new_guest_output.rtv))) {
      REXLOG_ERROR("D3D11Presenter: Failed to create the guest output {}x{} texture views",
                   frontbuffer_width, frontbuffer_height);
      return false;
    }
    if (uav_supported) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
      uav_desc.Format = kGuestOutputFormat;
      uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
      uav_desc.Texture2D.MipSlice = 0;
      if (FAILED(device->CreateUnorderedAccessView(new_guest_output.texture.Get(), &uav_desc,
                                                   &new_guest_output.uav))) {
        REXLOG_WARN("D3D11Presenter: Failed to create the guest output {}x{} UAV",
                    frontbuffer_width, frontbuffer_height);
        new_guest_output.uav.Reset();
      }
    }
    new_guest_output.width = frontbuffer_width;
    new_guest_output.height = frontbuffer_height;
    guest_output = std::move(new_guest_output);
  }
  D3D11GuestOutputRefreshContext context(is_8bpc_out_ref, guest_output.texture.Get(),
                                         guest_output.rtv.Get(), guest_output.uav.Get());
  return refresher(context);
}

bool D3D11Presenter::EnsureIntermediateTexture(IntermediateTexture& texture, uint32_t width,
                                               uint32_t height) {
  if (texture.texture && texture.width == width && texture.height == height) {
    return true;
  }
  texture = IntermediateTexture();
  ID3D11Device* device = provider_.GetDevice();
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = width;
  texture_desc.Height = height;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = kGuestOutputIntermediateFormat;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  IntermediateTexture new_texture;
  if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &new_texture.texture)) ||
      FAILED(device->CreateShaderResourceView(new_texture.texture.Get(), nullptr,
                                              &new_texture.srv)) ||
      FAILED(device->CreateRenderTargetView(new_texture.texture.Get(), nullptr,
                                            &new_texture.rtv))) {
    REXLOG_ERROR(
        "D3D11Presenter: Failed to create a {}x{} guest output presentation intermediate "
        "texture",
        width, height);
    return false;
  }
  new_texture.width = width;
  new_texture.height = height;
  texture = std::move(new_texture);
  return true;
}

bool D3D11Presenter::UpdateDynamicBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
                                         const void* data, size_t size) {
  // Deferred contexts allow only WRITE_DISCARD maps of dynamic resources.
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return false;
  }
  std::memcpy(mapped.pData, data, size);
  context->Unmap(buffer, 0);
  return true;
}

void D3D11Presenter::RecordPresentedCapture(ID3D11DeviceContext* context) {
  // Called with presented_capture_mutex_ held by the painting thread, which
  // keeps it until the command list containing the copy is executed.
  ID3D11Texture2D* back_buffer = paint_context_.back_buffer.Get();
  if (!back_buffer) {
    return;
  }
  D3D11_TEXTURE2D_DESC desc;
  back_buffer->GetDesc(&desc);
  if (presented_capture_texture_) {
    D3D11_TEXTURE2D_DESC capture_desc;
    presented_capture_texture_->GetDesc(&capture_desc);
    if (capture_desc.Width != desc.Width || capture_desc.Height != desc.Height) {
      presented_capture_texture_.Reset();
      presented_capture_written_ = false;
    }
  }
  if (!presented_capture_texture_) {
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(provider_.GetDevice()->CreateTexture2D(&desc, nullptr,
                                                      &presented_capture_texture_))) {
      presented_capture_texture_.Reset();
      return;
    }
  }
  context->CopyResource(presented_capture_texture_.Get(), back_buffer);
  presented_capture_written_ = true;
}

Presenter::PaintResult D3D11Presenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  ID3D11DeviceContext* context = paint_context_.deferred_context.Get();
  ID3D11RenderTargetView* back_buffer_rtv = paint_context_.back_buffer_rtv.Get();
  if (!paint_context_.swap_chain || !back_buffer_rtv) {
    return PaintResult::kNotPresentedConnectionOutdated;
  }
  const float kBackBufferClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool back_buffer_cleared = false;
  ID3D11ShaderResourceView* const null_srv = nullptr;

  // Draw the guest output.

  GuestOutputProperties guest_output_properties;
  GuestOutputPaintConfig guest_output_paint_config;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> guest_output_srv;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(ConsumeGuestOutput(
        guest_output_mailbox_index, &guest_output_properties, &guest_output_paint_config));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_srv = guest_output_textures_[guest_output_mailbox_index].srv;
    }
    // Incremented the reference count of the guest output view (and through
    // it the texture) - safe to leave the consumer critical section now.
  }

  SetPaintedGuestOutputRect(nullptr);
  if (guest_output_srv) {
    GuestOutputPaintFlow guest_output_flow = GetGuestOutputPaintFlow(
        guest_output_properties, paint_context_.swap_chain_width, paint_context_.swap_chain_height,
        D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        guest_output_paint_config);

    // Check if all guest output paint effects are supported by the
    // implementation. Unlike Direct3D 12 pipelines, pixel shaders don't depend
    // on the render target format, so one shader serves both the
    // intermediate and the final use.
    for (size_t i = 0; i < guest_output_flow.effect_count; ++i) {
      if (!guest_output_paint_ps_[size_t(guest_output_flow.effects[i])]) {
        guest_output_flow.effect_count = 0;
        break;
      }
    }

    // Make sure intermediate textures of the needed size are available, and
    // unneeded intermediate textures are destroyed (the runtime keeps them
    // alive for work already submitted).
    for (size_t i = 0; i + 1 < kMaxGuestOutputPaintEffects; ++i) {
      if (i + 1 < guest_output_flow.effect_count) {
        const std::pair<uint32_t, uint32_t>& size = guest_output_flow.effect_output_sizes[i];
        if (!EnsureIntermediateTexture(paint_context_.intermediate_textures[i], size.first,
                                       size.second)) {
          // Don't display the guest output.
          guest_output_flow.effect_count = 0;
        }
      } else {
        paint_context_.intermediate_textures[i] = IntermediateTexture();
      }
    }

    SetPaintedGuestOutputRect(&guest_output_flow);
    stats_render_width_.store(guest_output_properties.frontbuffer_width,
                              std::memory_order_relaxed);
    stats_render_height_.store(guest_output_properties.frontbuffer_height,
                               std::memory_order_relaxed);

    if (guest_output_flow.effect_count) {
      // Shared state of all passes. The deferred context starts each paint
      // with the default state.
      context->IASetInputLayout(nullptr);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context->VSSetShader(guest_output_paint_vs_.Get(), nullptr, 0);
      ID3D11Buffer* rect_constants = guest_output_paint_rect_constants_.Get();
      context->VSSetConstantBuffers(0, 1, &rect_constants);
      context->RSSetState(guest_output_paint_rasterizer_state_.Get());
      context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
      context->OMSetDepthStencilState(nullptr, 0);
      ID3D11Buffer* effect_constants_buffer = guest_output_paint_effect_constants_.Get();
      context->PSSetConstantBuffers(0, 1, &effect_constants_buffer);
      ID3D11SamplerState* sampler = guest_output_paint_sampler_.Get();
      context->PSSetSamplers(0, 1, &sampler);
    }

    for (size_t i = 0; i < guest_output_flow.effect_count; ++i) {
      bool is_final_effect = i + 1 >= guest_output_flow.effect_count;
      GuestOutputPaintEffect effect = guest_output_flow.effects[i];

      ID3D11RenderTargetView* dest_rtv;
      uint32_t dest_width, dest_height;
      int32_t effect_rect_x, effect_rect_y;
      if (is_final_effect) {
        // Clear the whole back buffer before the final pass: D3D11.0
        // ClearRenderTargetView takes no rectangles, and it also gives the
        // flip model's undefined buffer contents a defined letterbox.
        context->ClearRenderTargetView(back_buffer_rtv, kBackBufferClearColor);
        back_buffer_cleared = true;
        dest_rtv = back_buffer_rtv;
        dest_width = paint_context_.swap_chain_width;
        dest_height = paint_context_.swap_chain_height;
        effect_rect_x = guest_output_flow.output_x;
        effect_rect_y = guest_output_flow.output_y;
      } else {
        const IntermediateTexture& intermediate = paint_context_.intermediate_textures[i];
        dest_rtv = intermediate.rtv.Get();
        dest_width = intermediate.width;
        dest_height = intermediate.height;
        effect_rect_x = 0;
        effect_rect_y = 0;
      }

      // D3D11 silently unbinds a view whose resource gets bound for the other
      // direction. Unbind the previous source (it may be this destination),
      // bind the destination, and only then bind the source (the previous
      // pass's destination, no longer a render target at that point).
      context->PSSetShaderResources(0, 1, &null_srv);
      context->OMSetRenderTargets(1, &dest_rtv, nullptr);
      ID3D11ShaderResourceView* source_srv =
          i ? paint_context_.intermediate_textures[i - 1].srv.Get() : guest_output_srv.Get();
      context->PSSetShaderResources(0, 1, &source_srv);

      D3D11_VIEWPORT viewport;
      viewport.TopLeftX = 0.0f;
      viewport.TopLeftY = 0.0f;
      viewport.Width = float(dest_width);
      viewport.Height = float(dest_height);
      viewport.MinDepth = 0.0f;
      viewport.MaxDepth = 1.0f;
      context->RSSetViewports(1, &viewport);

      GuestOutputPaintRectangleConstants effect_rect_constants;
      float effect_x_to_ndc = 2.0f / viewport.Width;
      float effect_y_to_ndc = 2.0f / viewport.Height;
      effect_rect_constants.offset[0] = -1.0f + float(effect_rect_x) * effect_x_to_ndc;
      // +Y is -V.
      effect_rect_constants.offset[1] = 1.0f - float(effect_rect_y) * effect_y_to_ndc;
      effect_rect_constants.size[0] =
          float(guest_output_flow.effect_output_sizes[i].first) * effect_x_to_ndc;
      effect_rect_constants.size[1] =
          -float(guest_output_flow.effect_output_sizes[i].second) * effect_y_to_ndc;
      UpdateDynamicBuffer(context, guest_output_paint_rect_constants_.Get(),
                          &effect_rect_constants, sizeof(effect_rect_constants));

      // The D3D12 root constants of the effect, at offset 0 of the pixel
      // shader's b0 (the rest of the 32-byte buffer is unused).
      union {
        BilinearConstants bilinear;
#if defined(REX_HAS_FIDELITYFX_SDK)
        CasSharpenConstants cas_sharpen;
        CasResampleConstants cas_resample;
        FsrEasuConstants fsr_easu;
        FsrRcasConstants fsr_rcas;
#endif
        uint8_t bytes[kGuestOutputPaintEffectConstantsSize];
      } effect_constants = {};
      static_assert(sizeof(effect_constants) == kGuestOutputPaintEffectConstantsSize,
                    "An effect's constants outgrew the effect constant buffer");
      switch (effect) {
        case GuestOutputPaintEffect::kBilinear:
        case GuestOutputPaintEffect::kBilinearDither:
          effect_constants.bilinear.Initialize(guest_output_flow, i);
          break;
#if defined(REX_HAS_FIDELITYFX_SDK)
        case GuestOutputPaintEffect::kCasSharpen:
        case GuestOutputPaintEffect::kCasSharpenDither:
          effect_constants.cas_sharpen.Initialize(guest_output_flow, i,
                                                  guest_output_paint_config);
          break;
        case GuestOutputPaintEffect::kCasResample:
        case GuestOutputPaintEffect::kCasResampleDither:
          effect_constants.cas_resample.Initialize(guest_output_flow, i,
                                                   guest_output_paint_config);
          break;
        case GuestOutputPaintEffect::kFsrEasu:
          effect_constants.fsr_easu.Initialize(guest_output_flow, i);
          break;
        case GuestOutputPaintEffect::kFsrRcas:
        case GuestOutputPaintEffect::kFsrRcasDither:
          effect_constants.fsr_rcas.Initialize(guest_output_flow, i, guest_output_paint_config);
          break;
#endif
        default:
          assert_unhandled_case(effect);
          break;
      }
      UpdateDynamicBuffer(context, guest_output_paint_effect_constants_.Get(),
                          effect_constants.bytes, sizeof(effect_constants.bytes));

      context->PSSetShader(guest_output_paint_ps_[size_t(effect)].Get(), nullptr, 0);
      context->Draw(4, 0);
    }
  }

  if (!back_buffer_cleared) {
    context->ClearRenderTargetView(back_buffer_rtv, kBackBufferClearColor);
    back_buffer_cleared = true;
  }

  if (execute_ui_drawers) {
    // Draw the UI. Drawers set their own pipeline state.
    context->PSSetShaderResources(0, 1, &null_srv);
    context->OMSetRenderTargets(1, &back_buffer_rtv, nullptr);
    D3D11UIDrawContext ui_draw_context(*this, paint_context_.swap_chain_width,
                                       paint_context_.swap_chain_height, context,
                                       back_buffer_rtv);
    ExecuteUIDrawersFromUIThread(ui_draw_context);
  }

  // Leave nothing bound to the back buffer in the recorded list.
  context->PSSetShaderResources(0, 1, &null_srv);
  context->OMSetRenderTargets(0, nullptr, nullptr);

  // The presented-output copy, held locked from recording until the list is
  // on the immediate context so CapturePresentedOutput never maps the staging
  // texture while a copy to it is pending in an unexecuted list. Skipped for
  // this frame if a capture is reading it.
  std::unique_lock<std::mutex> presented_capture_lock(presented_capture_mutex_,
                                                      std::defer_lock);
  if (presented_capture_enabled_ && presented_capture_lock.try_lock()) {
    RecordPresentedCapture(context);
  }

  // Execute and present. FinishCommandList(FALSE) resets the deferred
  // context's state to the default; ExecuteCommandList(TRUE) restores the
  // command processor's immediate context state after the list, in one call.
  Microsoft::WRL::ComPtr<ID3D11CommandList> command_list;
  HRESULT finish_result = context->FinishCommandList(FALSE, &command_list);
  if (FAILED(finish_result)) {
    REXLOG_ERROR("D3D11Presenter: Failed to finish the painting command list (HRESULT 0x{:08X})",
                 static_cast<unsigned>(finish_result));
    if (presented_capture_lock.owns_lock()) {
      presented_capture_written_ = false;
    }
    HRESULT removed_reason = provider_.GetDevice()->GetDeviceRemovedReason();
    if (removed_reason == DXGI_ERROR_DEVICE_RESET) {
      return PaintResult::kGpuLostResponsible;
    }
    if (FAILED(removed_reason)) {
      return PaintResult::kGpuLostExternally;
    }
    return PaintResult::kNotPresented;
  }
  provider_.GetImmediateContext()->ExecuteCommandList(command_list.Get(), TRUE);
  command_list.Reset();
  if (presented_capture_lock.owns_lock()) {
    presented_capture_lock.unlock();
  }

  // Present as soon as possible, without waiting for vsync (the host refresh
  // rate may be something like 144 Hz, which is not a multiple of the common
  // 30 Hz or 60 Hz guest refresh rate), and allowing dropping outdated queued
  // frames for lower latency. Tearing (and variable refresh rate in
  // borderless fullscreen) only while vsync is off, as the Vulkan presenter
  // leaves out its tearing modes with vsync on. Not under the context lock:
  // Present may block on the frame queue, and it takes the device lock itself.
  UINT present_flags = DXGI_PRESENT_RESTART;
  if (paint_context_.swap_chain_allows_tearing && !IsVsyncRequested()) {
    present_flags |= DXGI_PRESENT_ALLOW_TEARING;
  }
  HRESULT present_result = paint_context_.swap_chain->Present(0, present_flags);
  if (present_result == DXGI_ERROR_DEVICE_REMOVED || present_result == DXGI_ERROR_DEVICE_RESET) {
    REXLOG_ERROR("D3D11 present failed: HRESULT 0x{:08X}, device removal reason 0x{:08X}",
                 static_cast<unsigned>(present_result),
                 static_cast<unsigned>(provider_.GetDevice()->GetDeviceRemovedReason()));
    // Presentation may report the loss before the command processor can log it.
    rex::FlushLogging();
  }
  switch (present_result) {
    case DXGI_ERROR_DEVICE_REMOVED:
      return PaintResult::kGpuLostExternally;
    case DXGI_ERROR_DEVICE_RESET:
      return PaintResult::kGpuLostResponsible;
    default:
      break;
  }
  if (FAILED(present_result)) {
    return PaintResult::kNotPresented;
  }
  stats_real_presents_.fetch_add(1, std::memory_order_relaxed);
  stats_output_width_.store(paint_context_.swap_chain_width, std::memory_order_relaxed);
  stats_output_height_.store(paint_context_.swap_chain_height, std::memory_order_relaxed);
  return PaintResult::kPresented;
}

bool D3D11Presenter::InitializeSurfaceIndependent() {
  // Check if DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING is supported.
  {
    Microsoft::WRL::ComPtr<IDXGIFactory5> dxgi_factory_5;
    if (SUCCEEDED(provider_.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&dxgi_factory_5)))) {
      BOOL tearing_feature_data = FALSE;
      dxgi_supports_tearing_ = SUCCEEDED(dxgi_factory_5->CheckFeatureSupport(
                                   DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_feature_data,
                                   sizeof(tearing_feature_data))) &&
                               tearing_feature_data;
    }
  }

  presented_capture_enabled_ = rex::cvar::GetFlagByName("present_capture_readable") == "true";

  ID3D11Device* device = provider_.GetDevice();

  // Guest output painting shaders.
  if (FAILED(device->CreateVertexShader(shaders::guest_output_triangle_strip_rect_vs,
                                        sizeof(shaders::guest_output_triangle_strip_rect_vs),
                                        nullptr, &guest_output_paint_vs_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output painting vertex shader");
    return false;
  }
  for (size_t i = 0; i < size_t(GuestOutputPaintEffect::kCount); ++i) {
    const void* bytecode;
    SIZE_T bytecode_size;
    switch (GuestOutputPaintEffect(i)) {
      case GuestOutputPaintEffect::kBilinear:
        bytecode = shaders::guest_output_bilinear_ps;
        bytecode_size = sizeof(shaders::guest_output_bilinear_ps);
        break;
      case GuestOutputPaintEffect::kBilinearDither:
        bytecode = shaders::guest_output_bilinear_dither_ps;
        bytecode_size = sizeof(shaders::guest_output_bilinear_dither_ps);
        break;
#if defined(REX_HAS_FIDELITYFX_SDK)
      case GuestOutputPaintEffect::kCasSharpen:
        bytecode = shaders::guest_output_ffx_cas_sharpen_ps;
        bytecode_size = sizeof(shaders::guest_output_ffx_cas_sharpen_ps);
        break;
      case GuestOutputPaintEffect::kCasSharpenDither:
        bytecode = shaders::guest_output_ffx_cas_sharpen_dither_ps;
        bytecode_size = sizeof(shaders::guest_output_ffx_cas_sharpen_dither_ps);
        break;
      case GuestOutputPaintEffect::kCasResample:
        bytecode = shaders::guest_output_ffx_cas_resample_ps;
        bytecode_size = sizeof(shaders::guest_output_ffx_cas_resample_ps);
        break;
      case GuestOutputPaintEffect::kCasResampleDither:
        bytecode = shaders::guest_output_ffx_cas_resample_dither_ps;
        bytecode_size = sizeof(shaders::guest_output_ffx_cas_resample_dither_ps);
        break;
      case GuestOutputPaintEffect::kFsrEasu:
        bytecode = shaders::guest_output_ffx_fsr_easu_ps;
        bytecode_size = sizeof(shaders::guest_output_ffx_fsr_easu_ps);
        break;
      case GuestOutputPaintEffect::kFsrRcas:
        bytecode = shaders::guest_output_ffx_fsr_rcas_ps;
        bytecode_size = sizeof(shaders::guest_output_ffx_fsr_rcas_ps);
        break;
      case GuestOutputPaintEffect::kFsrRcasDither:
        bytecode = shaders::guest_output_ffx_fsr_rcas_dither_ps;
        bytecode_size = sizeof(shaders::guest_output_ffx_fsr_rcas_dither_ps);
        break;
#endif
      default:
        // Not supported by this implementation.
        continue;
    }
    if (FAILED(device->CreatePixelShader(bytecode, bytecode_size, nullptr,
                                         &guest_output_paint_ps_[i]))) {
      REXLOG_ERROR("D3D11Presenter: Failed to create the guest output painting pixel shader {}",
                   i);
      return false;
    }
  }

  // Bilinear sampler (bilinear and EASU sample with it; CAS and RCAS only
  // load).
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MipLODBias = 0.0f;
  sampler_desc.MaxAnisotropy = 1;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MinLOD = 0.0f;
  sampler_desc.MaxLOD = 0.0f;
  if (FAILED(device->CreateSamplerState(&sampler_desc, &guest_output_paint_sampler_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output painting sampler");
    return false;
  }

  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  rasterizer_desc.ScissorEnable = FALSE;
  if (FAILED(device->CreateRasterizerState(&rasterizer_desc,
                                           &guest_output_paint_rasterizer_state_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output painting rasterizer state");
    return false;
  }

  // Dynamic constant buffers (the Direct3D 12 root constants).
  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.Usage = D3D11_USAGE_DYNAMIC;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  constants_desc.ByteWidth = UINT(rex::round_up(sizeof(GuestOutputPaintRectangleConstants), 16));
  if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &guest_output_paint_rect_constants_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output rectangle constant buffer");
    return false;
  }
  constants_desc.ByteWidth = kGuestOutputPaintEffectConstantsSize;
  if (FAILED(
          device->CreateBuffer(&constants_desc, nullptr, &guest_output_paint_effect_constants_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output effect constant buffer");
    return false;
  }

  // The painting deferred context (see the class comment).
  if (FAILED(device->CreateDeferredContext(0, &paint_context_.deferred_context))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the painting deferred context");
    return false;
  }

  return InitializeCommonSurfaceIndependent();
}

}  // namespace rex::ui::d3d11
