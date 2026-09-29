#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/memory/mapped_memory.h>

namespace rex::graphics {

// The FH1 precompiled shader pack (format v3, docs/native-renderer/
// SHADER_PACK_FORMAT.md in Pinyon Shift): translated guest shaders and the
// geometry shaders their pipelines need, for one backend, translator version,
// device feature set, translation flags and resolution scale.
class Fh1ShaderPack {
 public:
  static constexpr uint32_t kVersion = 3;

  enum class Backend : uint32_t {
    kD3D12 = 1,  // DXBC containers
    kVulkan = 2,  // SPIR-V modules
  };
  enum class Stage : uint32_t {
    kVertex = 1,
    kPixel = 2,
    // Host geometry shaders; the modification is the backend's geometry
    // shader key and the guest hash is zero.
    kGeometry = 3,
  };
  // Device features that change what the translator emits. D3D12: bit 0,
  // switch statements for control flow (not on Intel, where they are slow).
  static constexpr uint32_t kD3D12FeatureSwitch = 1u << 0;

  struct Config {
    uint32_t translator_version = 0;
    Backend backend = Backend::kD3D12;
    uint32_t device_features = 0;
    uint32_t flags = 0;
    uint32_t draw_resolution_scale_x = 1;
    uint32_t draw_resolution_scale_y = 1;

    bool operator==(const Config&) const = default;
  };

  struct TextureBinding {
    uint32_t bindless_descriptor_index = 0;
    uint32_t fetch_constant = 0;
    xenos::FetchOpDimension dimension = xenos::FetchOpDimension::k1D;
    bool is_signed = false;
  };
  struct SamplerBinding {
    uint32_t bindless_descriptor_index = 0;
    uint32_t fetch_constant = 0;
    xenos::TextureFilter mag_filter = xenos::TextureFilter::kPoint;
    xenos::TextureFilter min_filter = xenos::TextureFilter::kPoint;
    xenos::TextureFilter mip_filter = xenos::TextureFilter::kPoint;
    xenos::AnisoFilter aniso_filter = xenos::AnisoFilter::kDisabled;
  };

  struct Entry {
    Stage stage = Stage::kVertex;
    uint64_t guest_hash = 0;
    uint64_t modification = 0;
    // Valid until the owning pack is cleared or reloaded.
    std::span<const uint8_t> bytecode;
    std::vector<TextureBinding> texture_bindings;
    std::vector<SamplerBinding> sampler_bindings;
    uint32_t used_texture_mask = 0;
  };

  // The file name the runtime looks for under the shareable shader storage.
  static std::string FileName(uint32_t title_id, const Config& config);

  bool Load(const std::filesystem::path& path, const Config& expected_config,
            std::string* error_out = nullptr);
  const Entry* Find(Stage stage, uint64_t guest_hash, uint64_t modification) const;
  const Entry* Find(xenos::ShaderType stage, uint64_t guest_hash, uint64_t modification) const {
    return Find(stage == xenos::ShaderType::kVertex ? Stage::kVertex : Stage::kPixel, guest_hash,
                modification);
  }
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

}  // namespace rex::graphics
