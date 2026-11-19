/**
 ******************************************************************************
 * ShiftGlue: Direct3D 11 UI backend                                          *
 ******************************************************************************
 * Based on the Xenia Direct3D 12 immediate drawer, Copyright 2022 Ben Vanik. *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <rex/ui/d3d11/d3d11_api.h>
#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui::d3d11 {

// Draws on the context of the D3D11UIDrawContext it's given (the presenter's
// deferred context), never on the provider's immediate context. Textures are
// created with their data (a free-threaded device call), and the D3D11
// runtime keeps resources alive for recorded and submitted work, so there's
// no upload queue or delayed deletion as on Direct3D 12.
class D3D11ImmediateDrawer final : public ImmediateDrawer {
 public:
  static std::unique_ptr<D3D11ImmediateDrawer> Create(const D3D11Provider& provider) {
    auto immediate_drawer =
        std::unique_ptr<D3D11ImmediateDrawer>(new D3D11ImmediateDrawer(provider));
    if (!immediate_drawer->Initialize()) {
      return nullptr;
    }
    return immediate_drawer;
  }

  ~D3D11ImmediateDrawer();

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;

  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  enum class SamplerIndex {
    kNearestClamp,
    kLinearClamp,
    kNearestRepeat,
    kLinearRepeat,

    kCount,
    kInvalid = kCount
  };

  class D3D11ImmediateTexture final : public ImmediateTexture {
   public:
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11ImmediateTexture(uint32_t width, uint32_t height, ID3D11ShaderResourceView* srv,
                          SamplerIndex sampler_index)
        : ImmediateTexture(width, height), srv_(srv), sampler_index_(sampler_index) {}

    // nullptr if creation failed (drawn as solid color then).
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    SamplerIndex sampler_index() const { return sampler_index_; }

   private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    SamplerIndex sampler_index_;
  };

  D3D11ImmediateDrawer(const D3D11Provider& provider) : provider_(provider) {}
  bool Initialize();

  // Grows a dynamic buffer to hold at least size bytes and fills it with
  // WRITE_DISCARD (the only map deferred contexts allow).
  bool UploadToDynamicBuffer(ID3D11DeviceContext* context,
                             Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, UINT& buffer_size,
                             UINT bind_flags, const void* data, UINT size);

  ID3D11DeviceContext* context() const;

  const D3D11Provider& provider_;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_state_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> samplers_[size_t(SamplerIndex::kCount)];
  // 1x1 opaque white, bound for untextured (solid color) draws: D3D11 views
  // have no constant component mapping like Direct3D 12's FORCE_VALUE_1.
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> white_texture_srv_;
  // Dynamic, the coordinate space size inverse (b0 of the vertex shader).
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
  UINT vertex_buffer_size_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer_;
  UINT index_buffer_size_ = 0;

  bool batch_open_ = false;
  bool batch_has_index_buffer_ = false;
  D3D11_RECT current_scissor_ = {};
  D3D11_PRIMITIVE_TOPOLOGY current_primitive_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11ShaderResourceView* current_srv_ = nullptr;
  SamplerIndex current_sampler_index_ = SamplerIndex::kInvalid;
};

}  // namespace rex::ui::d3d11
