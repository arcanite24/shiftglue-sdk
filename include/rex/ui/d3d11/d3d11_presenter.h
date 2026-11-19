/**
 ******************************************************************************
 * ShiftGlue: Direct3D 11 UI backend                                          *
 ******************************************************************************
 * Based on the Xenia Direct3D 12 presenter, Copyright 2022 Ben Vanik.        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>

namespace rex::ui::d3d11 {

// What UI drawers (the immediate drawer) draw with: the presenter's deferred
// context, with the swap chain back buffer bound as the only render target.
// Drawers must set all the pipeline state they need (the deferred context
// starts every paint with the default state) and must never touch the
// provider's immediate context.
class D3D11UIDrawContext final : public UIDrawContext {
 public:
  D3D11UIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height, ID3D11DeviceContext* context,
                     ID3D11RenderTargetView* render_target_view)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        context_(context),
        render_target_view_(render_target_view) {}

  ID3D11DeviceContext* context() const { return context_; }
  ID3D11RenderTargetView* render_target_view() const { return render_target_view_; }

 private:
  ID3D11DeviceContext* context_;
  ID3D11RenderTargetView* render_target_view_;
};

// Direct3D 11 presenter (feature level 11_0).
//
// Painting (guest output effect passes, letterbox clear, UI drawers) is
// recorded on a deferred context owned by the presenter and replayed on the
// immediate context with one ExecuteCommandList(list, TRUE) call followed by
// Present, so the presenter never binds state on the immediate context that
// the GPU command processor owns (see the threading contract in
// d3d11_provider.h). Painting may run on the UI thread or on the guest output
// refresh thread (host_present_from_non_ui_thread); the Presenter base class
// lets only one of them paint at a time.
//
// Kept from the Direct3D 12 presenter: the guest output mailbox, the spatial
// effects (bilinear, FidelityFX CAS sharpen and resample, FSR EASU + RCAS,
// 8bpc dither), UI drawers, CaptureGuestOutput, VRR/tearing.
// Added (the Vulkan presenter has them): CapturePresentedOutput (with
// present_capture_readable) and GetPresentStatsFromUIThread.
// Left out: the FidelityFX runtime temporal upscalers (FSR 2/3), DLSS and
// frame generation, which need Direct3D 12 or Vulkan (a temporal effect is
// painted with its FSR 1 / spatial fallback, as on Direct3D 12 without the
// runtime).
// TODO(D3D11 HDR): the Direct3D 12 presenter has no HDR output to port; the
// Vulkan one does (present_hdr). On DXGI it would map to an
// R16G16B16A16_FLOAT flip-model swap chain with
// DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 (IDXGISwapChain3::SetColorSpace1),
// once hdr_output.ps.glsl exists as an SM 5.0 shader.
class D3D11Presenter final : public Presenter {
 public:
  // Guest output (mailbox) texture: DXGI_FORMAT_R10G10B10A2_UNORM,
  // D3D11_USAGE_DEFAULT, 1 mip, 1 sample, no CPU access, no misc flags,
  // bind flags kGuestOutputBindFlags (plus D3D11_BIND_UNORDERED_ACCESS when the
  // provider reports typed UAV stores for the format).
  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr UINT kGuestOutputBindFlags =
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  static constexpr DXGI_FORMAT kGuestOutputIntermediateFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

  // The format used internally by Windows composition.
  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

  // Given to the refresher passed to Presenter::RefreshGuestOutput.
  //
  // Contract for the refresher (the GPU command processor):
  // - It runs on the thread that called RefreshGuestOutput (the command
  //   processor's), and writes the whole texture (frontbuffer_width x
  //   frontbuffer_height, the texture's size) with commands on the provider's
  //   immediate context, which it owns, before returning. No fence or
  //   transition is needed: the presenter's command list for painting is
  //   executed on the same immediate context later, so the runtime orders it
  //   after the refresher's writes.
  // - It must unbind the texture from its output-merger and UAV slots before
  //   returning (or at least before its next draw that does not write it),
  //   because the presenter will read it as a shader resource; D3D11 resolves
  //   read / write hazards by silently unbinding.
  // - It must not keep the views or the texture beyond the callback: the
  //   presenter recreates them when the frontbuffer size changes. The views
  //   below are created once per texture by the presenter, so it doesn't
  //   need to create its own.
  class D3D11GuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    D3D11GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D11Texture2D* texture,
                                   ID3D11RenderTargetView* rtv, ID3D11UnorderedAccessView* uav)
        : GuestOutputRefreshContext(is_8bpc_out_ref), texture_(texture), rtv_(rtv), uav_(uav) {}

    // kGuestOutputFormat, see kGuestOutputBindFlags for the description.
    ID3D11Texture2D* texture() const { return texture_; }
    DXGI_FORMAT format() const { return kGuestOutputFormat; }
    // Texture2D RTV of mip 0 in kGuestOutputFormat.
    ID3D11RenderTargetView* rtv() const { return rtv_; }
    // Texture2D UAV of mip 0 in kGuestOutputFormat (typed store), or nullptr
    // if the device lacks typed UAV stores for the format
    // (D3D11Provider::IsGuestOutputFormatUAVStoreSupported) - draw through
    // the RTV then.
    ID3D11UnorderedAccessView* uav_or_null() const { return uav_; }

   private:
    ID3D11Texture2D* texture_;
    ID3D11RenderTargetView* rtv_;
    ID3D11UnorderedAccessView* uav_;
  };

  static std::unique_ptr<D3D11Presenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                const D3D11Provider& provider) {
    auto presenter =
        std::unique_ptr<D3D11Presenter>(new D3D11Presenter(host_gpu_loss_callback, provider));
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~D3D11Presenter();

  const D3D11Provider& provider() const { return provider_; }

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;
  bool CapturePresentedOutput(RawImage& image_out) override;

  PresentStats GetPresentStatsFromUIThread() const override;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  // The vertex shader's cbuffer (b0), the D3D12 rectangle root constants.
  struct GuestOutputPaintRectangleConstants {
    float offset[2];
    float size[2];
  };

  struct GuestOutputTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  struct IntermediateTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  enum class SwapEffect {
    kFlipDiscard,
    kFlipSequential,
    kDiscard,
  };

  // Accessible only by painting and by surface connection lifetime management
  // by the thread doing it, as well as by presenter initialization and
  // shutdown.
  struct PaintContext {
    // Flip model needs at least 2; 3 like the Direct3D 12 presenter.
    static constexpr uint32_t kFlipSwapChainBufferCount = 3;
    static constexpr uint32_t kBltSwapChainBufferCount = 2;

    // Flushes the immediate context afterwards: D3D11 defers object destruction,
    // and a new flip-model swap chain fails on an HWND the old one still owns.
    void DestroySwapChain(ID3D11DeviceContext* immediate_context);

    // Connection-independent.
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred_context;
    std::array<IntermediateTexture, kMaxGuestOutputPaintEffects - 1> intermediate_textures;

    // Connection-specific.
    uint32_t swap_chain_width = 0;
    uint32_t swap_chain_height = 0;
    UINT swap_chain_flags = 0;
    SwapEffect swap_effect = SwapEffect::kFlipDiscard;
    bool swap_chain_allows_tearing = false;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain;
    // In D3D11 buffer 0 is always the current back buffer, for every model.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> back_buffer_rtv;
  };

  explicit D3D11Presenter(HostGpuLossCallback host_gpu_loss_callback, const D3D11Provider& provider)
      : Presenter(host_gpu_loss_callback), provider_(provider) {}

  bool InitializeSurfaceIndependent();

  bool EnsureIntermediateTexture(IntermediateTexture& texture, uint32_t width, uint32_t height);
  bool UpdateDynamicBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data,
                           size_t size);
  void RecordPresentedCapture(ID3D11DeviceContext* context);

  const D3D11Provider& provider_;

  // Whether DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING is supported by DXGI (depends in
  // particular on the Windows 10 version and hardware support), primarily for
  // variable refresh rate support.
  bool dxgi_supports_tearing_ = false;

  // Static objects for guest output painting.
  Microsoft::WRL::ComPtr<ID3D11VertexShader> guest_output_paint_vs_;
  std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, size_t(GuestOutputPaintEffect::kCount)>
      guest_output_paint_ps_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> guest_output_paint_sampler_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> guest_output_paint_rasterizer_state_;
  // Dynamic, written with WRITE_DISCARD for each pass.
  Microsoft::WRL::ComPtr<ID3D11Buffer> guest_output_paint_rect_constants_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> guest_output_paint_effect_constants_;

  // Indexed by the mailbox index. Written by the refresher thread for the
  // image it owns in the mailbox, read by painting and capturing under the
  // guest output consumer lock (copying the references out).
  std::array<GuestOutputTexture, kGuestOutputMailboxSize> guest_output_textures_;

  PaintContext paint_context_;

  // CapturePresentedOutput: a staging copy of the latest painted back buffer,
  // written by the painting command list and read under the provider's
  // context lock.
  // present_capture_readable (owned by the Vulkan presenter, so read by name;
  // restart-only, so read once at initialization).
  bool presented_capture_enabled_ = false;
  std::mutex presented_capture_mutex_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> presented_capture_texture_;
  bool presented_capture_written_ = false;

  // For GetPresentStatsFromUIThread (painting may be on another thread).
  std::atomic<uint64_t> stats_real_presents_{0};
  std::atomic<uint32_t> stats_render_width_{0};
  std::atomic<uint32_t> stats_render_height_{0};
  std::atomic<uint32_t> stats_output_width_{0};
  std::atomic<uint32_t> stats_output_height_{0};
};

}  // namespace rex::ui::d3d11
