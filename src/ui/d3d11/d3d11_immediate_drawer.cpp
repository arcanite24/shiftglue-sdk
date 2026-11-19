/**
 ******************************************************************************
 * ShiftGlue: Direct3D 11 UI backend                                          *
 ******************************************************************************
 * Based on the Xenia Direct3D 12 immediate drawer, Copyright 2022 Ben Vanik. *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <memory>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d11/d3d11_immediate_drawer.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

namespace rex::ui::d3d11 {

// Generated with src/ui/shaders/compile-d3d11.sh.
namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/immediate_ps.h"
#include "../shaders/bytecode/d3d11_5_0/immediate_vs.h"
}  // namespace shaders

D3D11ImmediateDrawer::~D3D11ImmediateDrawer() = default;

bool D3D11ImmediateDrawer::Initialize() {
  ID3D11Device* device = provider_.GetDevice();

  if (FAILED(device->CreateVertexShader(shaders::immediate_vs, sizeof(shaders::immediate_vs),
                                        nullptr, &vertex_shader_)) ||
      FAILED(device->CreatePixelShader(shaders::immediate_ps, sizeof(shaders::immediate_ps),
                                       nullptr, &pixel_shader_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the shaders");
    return false;
  }

  D3D11_INPUT_ELEMENT_DESC input_elements[3] = {};
  input_elements[0].SemanticName = "POSITION";
  input_elements[0].Format = DXGI_FORMAT_R32G32_FLOAT;
  input_elements[0].AlignedByteOffset = offsetof(ImmediateVertex, x);
  input_elements[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
  input_elements[1].SemanticName = "TEXCOORD";
  input_elements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
  input_elements[1].AlignedByteOffset = offsetof(ImmediateVertex, u);
  input_elements[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
  input_elements[2].SemanticName = "COLOR";
  input_elements[2].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  input_elements[2].AlignedByteOffset = offsetof(ImmediateVertex, color);
  input_elements[2].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
  if (FAILED(device->CreateInputLayout(input_elements, UINT(rex::countof(input_elements)),
                                       shaders::immediate_vs, sizeof(shaders::immediate_vs),
                                       &input_layout_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the input layout");
    return false;
  }

  D3D11_BLEND_DESC blend_desc = {};
  D3D11_RENDER_TARGET_BLEND_DESC& rt_blend_desc = blend_desc.RenderTarget[0];
  rt_blend_desc.BlendEnable = TRUE;
  rt_blend_desc.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  rt_blend_desc.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  rt_blend_desc.BlendOp = D3D11_BLEND_OP_ADD;
  rt_blend_desc.SrcBlendAlpha = D3D11_BLEND_ONE;
  rt_blend_desc.DestBlendAlpha = D3D11_BLEND_ONE;
  rt_blend_desc.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  rt_blend_desc.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->CreateBlendState(&blend_desc, &blend_state_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the blend state");
    return false;
  }

  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.FrontCounterClockwise = FALSE;
  rasterizer_desc.DepthClipEnable = TRUE;
  // Direct3D 12 always scissors; D3D11 only with this.
  rasterizer_desc.ScissorEnable = TRUE;
  if (FAILED(device->CreateRasterizerState(&rasterizer_desc, &rasterizer_state_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the rasterizer state");
    return false;
  }

  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
  depth_stencil_desc.DepthEnable = FALSE;
  depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depth_stencil_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  depth_stencil_desc.StencilEnable = FALSE;
  if (FAILED(device->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the depth / stencil state");
    return false;
  }

  // Samplers.
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.MaxAnisotropy = 1;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MinLOD = 0.0f;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  struct {
    SamplerIndex index;
    D3D11_FILTER filter;
    D3D11_TEXTURE_ADDRESS_MODE address;
  } sampler_setups[] = {
      {SamplerIndex::kNearestClamp, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP},
      {SamplerIndex::kLinearClamp, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP},
      {SamplerIndex::kNearestRepeat, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP},
      {SamplerIndex::kLinearRepeat, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP},
  };
  for (const auto& setup : sampler_setups) {
    sampler_desc.Filter = setup.filter;
    sampler_desc.AddressU = setup.address;
    sampler_desc.AddressV = setup.address;
    sampler_desc.AddressW = setup.address;
    if (FAILED(device->CreateSamplerState(&sampler_desc, &samplers_[size_t(setup.index)]))) {
      REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create a sampler");
      return false;
    }
  }

  // 1x1 white texture for solid color drawing.
  {
    const uint32_t white = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = 1;
    texture_desc.Height = 1;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = D3D11ImmediateTexture::kFormat;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA texture_data = {};
    texture_data.pSysMem = &white;
    texture_data.SysMemPitch = sizeof(white);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&texture_desc, &texture_data, &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &white_texture_srv_))) {
      REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the solid color texture");
      return false;
    }
  }

  D3D11_BUFFER_DESC constant_buffer_desc = {};
  constant_buffer_desc.ByteWidth = 16;
  constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
  constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&constant_buffer_desc, nullptr, &constant_buffer_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the constant buffer");
    return false;
  }

  batch_open_ = false;
  return true;
}

std::unique_ptr<ImmediateTexture> D3D11ImmediateDrawer::CreateTexture(uint32_t width,
                                                                      uint32_t height,
                                                                      ImmediateTextureFilter filter,
                                                                      bool is_repeated,
                                                                      const uint8_t* data) {
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = width;
  texture_desc.Height = height;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = D3D11ImmediateTexture::kFormat;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA texture_data = {};
  texture_data.pSysMem = data;
  texture_data.SysMemPitch = UINT(sizeof(uint32_t) * width);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  // Free-threaded device calls, no context needed.
  if (width && height &&
      SUCCEEDED(provider_.GetDevice()->CreateTexture2D(&texture_desc, data ? &texture_data : nullptr,
                                                       &texture))) {
    if (FAILED(provider_.GetDevice()->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
      REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create a view of a {}x{} texture", width,
                   height);
      srv.Reset();
    }
  } else {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create a {}x{} texture", width, height);
  }

  SamplerIndex sampler_index;
  if (filter == ImmediateTextureFilter::kLinear) {
    sampler_index = is_repeated ? SamplerIndex::kLinearRepeat : SamplerIndex::kLinearClamp;
  } else {
    sampler_index = is_repeated ? SamplerIndex::kNearestRepeat : SamplerIndex::kNearestClamp;
  }
  return std::make_unique<D3D11ImmediateTexture>(width, height, srv.Get(), sampler_index);
}

ID3D11DeviceContext* D3D11ImmediateDrawer::context() const {
  return static_cast<const D3D11UIDrawContext*>(ui_draw_context())->context();
}

void D3D11ImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                                 float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);

  assert_false(batch_open_);

  const D3D11UIDrawContext& d3d11_ui_draw_context =
      static_cast<const D3D11UIDrawContext&>(ui_draw_context);
  ID3D11DeviceContext* context = d3d11_ui_draw_context.context();

  // Set all the state: the context may have anything bound by the presenter's
  // guest output passes or another drawer.
  ID3D11RenderTargetView* render_target_view = d3d11_ui_draw_context.render_target_view();
  context->OMSetRenderTargets(1, &render_target_view, nullptr);
  context->OMSetBlendState(blend_state_.Get(), nullptr, UINT_MAX);
  context->OMSetDepthStencilState(depth_stencil_state_.Get(), 0);
  context->RSSetState(rasterizer_state_.Get());

  D3D11_VIEWPORT viewport;
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = float(d3d11_ui_draw_context.render_target_width());
  viewport.Height = float(d3d11_ui_draw_context.render_target_height());
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context->RSSetViewports(1, &viewport);

  context->IASetInputLayout(input_layout_.Get());
  context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context->PSSetShader(pixel_shader_.Get(), nullptr, 0);
  context->GSSetShader(nullptr, nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);

  // ImmediateDrawer::Begin replaces a non-positive coordinate space with the
  // render target size.
  float coordinate_space_size_inv[4] = {1.0f / this->coordinate_space_width(),
                                        1.0f / this->coordinate_space_height(), 0.0f, 0.0f};
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(context->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    std::memcpy(mapped.pData, coordinate_space_size_inv, sizeof(coordinate_space_size_inv));
    context->Unmap(constant_buffer_.Get(), 0);
  }
  ID3D11Buffer* constant_buffer = constant_buffer_.Get();
  context->VSSetConstantBuffers(0, 1, &constant_buffer);

  current_scissor_.left = 0;
  current_scissor_.top = 0;
  current_scissor_.right = 0;
  current_scissor_.bottom = 0;
  context->RSSetScissorRects(1, &current_scissor_);

  current_primitive_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  current_srv_ = nullptr;
  current_sampler_index_ = SamplerIndex::kInvalid;
}

bool D3D11ImmediateDrawer::UploadToDynamicBuffer(ID3D11DeviceContext* context,
                                                 Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer,
                                                 UINT& buffer_size, UINT bind_flags,
                                                 const void* data, UINT size) {
  if (!buffer || buffer_size < size) {
    UINT new_size = std::max(UINT(64 * 1024), buffer_size);
    while (new_size < size) {
      new_size *= 2;
    }
    D3D11_BUFFER_DESC buffer_desc = {};
    buffer_desc.ByteWidth = new_size;
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = bind_flags;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    // The old buffer stays alive for the command lists referencing it.
    buffer.Reset();
    buffer_size = 0;
    if (FAILED(provider_.GetDevice()->CreateBuffer(&buffer_desc, nullptr, &buffer))) {
      return false;
    }
    buffer_size = new_size;
  }
  // WRITE_DISCARD per batch: the only map deferred contexts allow, and each
  // discard gets fresh memory, so earlier batches in the list are unaffected.
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return false;
  }
  std::memcpy(mapped.pData, data, size);
  context->Unmap(buffer.Get(), 0);
  return true;
}

void D3D11ImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  assert_false(batch_open_);
  if (batch.vertex_count <= 0) {
    return;
  }

  ID3D11DeviceContext* context = this->context();

  // Bind the vertices.
  UINT vertex_data_size = UINT(sizeof(ImmediateVertex)) * UINT(batch.vertex_count);
  if (!UploadToDynamicBuffer(context, vertex_buffer_, vertex_buffer_size_,
                             D3D11_BIND_VERTEX_BUFFER, batch.vertices, vertex_data_size)) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to get a buffer for {} vertices",
                 batch.vertex_count);
    return;
  }
  ID3D11Buffer* vertex_buffer = vertex_buffer_.Get();
  UINT vertex_stride = UINT(sizeof(ImmediateVertex));
  UINT vertex_offset = 0;
  context->IASetVertexBuffers(0, 1, &vertex_buffer, &vertex_stride, &vertex_offset);

  // Bind the indices.
  batch_has_index_buffer_ = batch.indices != nullptr;
  if (batch_has_index_buffer_) {
    UINT index_data_size = UINT(sizeof(uint16_t)) * UINT(batch.index_count);
    if (!index_data_size ||
        !UploadToDynamicBuffer(context, index_buffer_, index_buffer_size_,
                               D3D11_BIND_INDEX_BUFFER, batch.indices, index_data_size)) {
      REXLOG_ERROR("D3D11ImmediateDrawer: Failed to get a buffer for {} indices",
                   batch.index_count);
      return;
    }
    context->IASetIndexBuffer(index_buffer_.Get(), DXGI_FORMAT_R16_UINT, 0);
  }

  batch_open_ = true;
}

void D3D11ImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!batch_open_) {
    // Could be an error while obtaining the vertex and index buffers.
    return;
  }

  ID3D11DeviceContext* context = this->context();

  // Set the scissor rectangle.
  uint32_t scissor_left, scissor_top, scissor_width, scissor_height;
  if (!ScissorToRenderTarget(draw, scissor_left, scissor_top, scissor_width, scissor_height)) {
    // Nothing is visible (zero area is used as the default current_scissor_
    // value also).
    return;
  }
  D3D11_RECT scissor;
  scissor.left = LONG(scissor_left);
  scissor.top = LONG(scissor_top);
  scissor.right = LONG(scissor_left + scissor_width);
  scissor.bottom = LONG(scissor_top + scissor_height);
  if (current_scissor_.left != scissor.left || current_scissor_.top != scissor.top ||
      current_scissor_.right != scissor.right || current_scissor_.bottom != scissor.bottom) {
    current_scissor_ = scissor;
    context->RSSetScissorRects(1, &scissor);
  }

  // Bind the texture, or the white one for solid color (and for a texture
  // whose creation failed). Use nearest-neighbor and clamp for solid color so
  // fetching is simpler.
  auto texture = static_cast<D3D11ImmediateTexture*>(draw.texture);
  ID3D11ShaderResourceView* srv = texture ? texture->srv() : nullptr;
  SamplerIndex sampler_index = srv ? texture->sampler_index() : SamplerIndex::kNearestClamp;
  if (!srv) {
    srv = white_texture_srv_.Get();
  }
  if (current_srv_ != srv) {
    current_srv_ = srv;
    context->PSSetShaderResources(0, 1, &srv);
  }
  if (current_sampler_index_ != sampler_index) {
    current_sampler_index_ = sampler_index;
    ID3D11SamplerState* sampler = samplers_[size_t(sampler_index)].Get();
    context->PSSetSamplers(0, 1, &sampler);
  }

  // Set the primitive type.
  D3D11_PRIMITIVE_TOPOLOGY primitive_topology;
  switch (draw.primitive_type) {
    case ImmediatePrimitiveType::kLines:
      primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
      break;
    case ImmediatePrimitiveType::kTriangles:
      primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    default:
      assert_unhandled_case(draw.primitive_type);
      return;
  }
  if (current_primitive_topology_ != primitive_topology) {
    current_primitive_topology_ = primitive_topology;
    context->IASetPrimitiveTopology(primitive_topology);
  }

  // Draw.
  if (batch_has_index_buffer_) {
    context->DrawIndexed(UINT(draw.count), UINT(draw.index_offset), INT(draw.base_vertex));
  } else {
    context->Draw(UINT(draw.count), UINT(draw.base_vertex));
  }
}

void D3D11ImmediateDrawer::EndDrawBatch() { batch_open_ = false; }

void D3D11ImmediateDrawer::End() {
  assert_false(batch_open_);

  // Don't leave the drawer's texture bound for whatever the context records
  // next.
  if (ui_draw_context()) {
    ID3D11ShaderResourceView* const null_srv = nullptr;
    context()->PSSetShaderResources(0, 1, &null_srv);
  }
  current_srv_ = nullptr;

  ImmediateDrawer::End();
}

}  // namespace rex::ui::d3d11
