/**
 * @file        graphics/d3d11/shader.h
 * @brief       Direct3D 11 guest shader and its host translations.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11Shader : public DxbcShader {
 public:
  class D3D11Translation : public DxbcTranslation {
   public:
    D3D11Translation(D3D11Shader& shader, uint64_t modification)
        : DxbcTranslation(shader, modification) {}

    // Created by the pipeline cache from the translated shader model 5.0
    // DXBC. One of them, depending on the guest shader type and the host
    // vertex shader type (only vertex and pixel shaders are used: FH1 draws
    // no tessellated primitives on the D3D11 path).
    ID3D11VertexShader* vertex_shader() const { return vertex_shader_.Get(); }
    ID3D11PixelShader* pixel_shader() const { return pixel_shader_.Get(); }
    // Whether creation of the host shader has been attempted (successfully or
    // not), to not retry a failed creation on every draw.
    bool host_shader_creation_attempted() const { return host_shader_creation_attempted_; }

   private:
    friend class D3D11PipelineCache;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    bool host_shader_creation_attempted_ = false;
  };

  D3D11Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
              size_t ucode_dword_count, std::endian ucode_source_endian = std::endian::big)
      : DxbcShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                   ucode_source_endian) {}

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override {
    return new D3D11Translation(*this, modification);
  }
};

}  // namespace rex::graphics::d3d11
