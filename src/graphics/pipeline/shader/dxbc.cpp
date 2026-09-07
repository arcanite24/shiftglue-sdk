/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <thread>

#include <rex/graphics/pipeline/shader/dxbc.h>

namespace rex::graphics {

DxbcShader::DxbcShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                       const uint32_t* ucode_dwords, size_t ucode_dword_count,
                       std::endian ucode_source_endian)
    : Shader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count, ucode_source_endian) {}

bool DxbcShader::LoadPrecompiledBindings(std::span<const TextureBinding> texture_bindings,
                                         std::span<const SamplerBinding> sampler_bindings,
                                         uint32_t used_texture_mask) {
  if (!bindings_setup_entered_.test_and_set(std::memory_order_acq_rel)) {
    texture_bindings_.assign(texture_bindings.begin(), texture_bindings.end());
    sampler_bindings_.assign(sampler_bindings.begin(), sampler_bindings.end());
    used_texture_mask_ = used_texture_mask;
    bindings_setup_complete_.store(true, std::memory_order_release);
    return true;
  }
  while (!bindings_setup_complete_.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  return used_texture_mask_ == used_texture_mask &&
         texture_bindings_.size() == texture_bindings.size() &&
         sampler_bindings_.size() == sampler_bindings.size() &&
         (texture_bindings.empty() ||
          !std::memcmp(texture_bindings_.data(), texture_bindings.data(),
                       texture_bindings.size_bytes())) &&
         (sampler_bindings.empty() ||
          !std::memcmp(sampler_bindings_.data(), sampler_bindings.data(),
                       sampler_bindings.size_bytes()));
}

Shader::Translation* DxbcShader::CreateTranslationInstance(uint64_t modification) {
  return new DxbcTranslation(*this, modification);
}

}  // namespace rex::graphics
