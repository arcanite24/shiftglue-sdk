#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/xenos.h>
#include <rex/memory/mapped_memory.h>

namespace rex::graphics::d3d12 {

class Fh1ShaderPack {
 public:
  struct Config {
    uint32_t translator_version = 0;
    uint32_t vendor_id = 0;
    uint32_t flags = 0;
    uint32_t draw_resolution_scale_x = 1;
    uint32_t draw_resolution_scale_y = 1;

    bool operator==(const Config&) const = default;
  };

  struct Entry {
    xenos::ShaderType stage = xenos::ShaderType::kVertex;
    uint64_t guest_hash = 0;
    uint64_t modification = 0;
    // Valid until the owning pack is cleared or reloaded.
    std::span<const uint8_t> bytecode;
    std::vector<DxbcShader::TextureBinding> texture_bindings;
    std::vector<DxbcShader::SamplerBinding> sampler_bindings;
    uint32_t used_texture_mask = 0;
  };

  bool Load(const std::filesystem::path& path, const Config& expected_config,
            std::string* error_out = nullptr);
  const Entry* Find(xenos::ShaderType stage, uint64_t guest_hash,
                    uint64_t modification) const;
  const std::vector<Entry>& entries() const { return entries_; }
  size_t size() const { return entries_.size(); }
  void Clear() {
    entries_.clear();
    mapping_.reset();
  }

 private:
  std::unique_ptr<rex::memory::MappedMemory> mapping_;
  std::vector<Entry> entries_;
};

}  // namespace rex::graphics::d3d12
