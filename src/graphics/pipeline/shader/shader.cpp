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

#include <cinttypes>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/string.h>

namespace rex::graphics {
using namespace ucode;

Shader::Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
               const uint32_t* ucode_dwords, size_t ucode_dword_count,
               std::endian ucode_source_endian)
    : shader_type_(shader_type), ucode_data_hash_(ucode_data_hash) {
  // We keep ucode data in host native format so it's easier to work with.
  ucode_data_.resize(ucode_dword_count);
  if (std::endian::native != ucode_source_endian) {
    memory::copy_and_swap(ucode_data_.data(), ucode_dwords, ucode_dword_count);
  } else {
    std::memcpy(ucode_data_.data(), ucode_dwords, sizeof(uint32_t) * ucode_dword_count);
  }
}

Shader::~Shader() {
  for (auto it : translations_) {
    delete it.second;
  }
}

uint32_t Shader::GetInterpolatorInputMask(reg::SQ_PROGRAM_CNTL sq_program_cntl,
                                          reg::SQ_CONTEXT_MISC sq_context_misc,
                                          uint32_t& param_gen_pos_out) const {
  assert_true(type() == xenos::ShaderType::kPixel);
  uint32_t interpolator_count =
      std::min(xenos::kMaxInterpolators,
               std::max(register_static_address_bound(),
                        GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
  uint32_t interpolator_mask = (UINT32_C(1) << interpolator_count) - 1;
  if (sq_program_cntl.param_gen && sq_context_misc.param_gen_pos < interpolator_count) {
    interpolator_mask &= ~(UINT32_C(1) << sq_context_misc.param_gen_pos);
    param_gen_pos_out = sq_context_misc.param_gen_pos;
  } else {
    param_gen_pos_out = UINT32_MAX;
  }
  return interpolator_mask;
}

bool Shader::LoadRuntimeAnalysis(RuntimeAnalysis analysis) {
  if (is_ucode_analyzed_ || analysis.vertex_bindings.size() > 96 ||
      analysis.constant_register_map.float_count > 256 ||
      (analysis.writes_interpolators & ~((uint32_t(1) << xenos::kMaxInterpolators) - 1)) ||
      (analysis.writes_point_size_edge_flag_kill_vertex & ~uint32_t(0b111)) ||
      (analysis.writes_color_targets & ~uint32_t(0b1111)) ||
      (analysis.memexport_eM_written & ~uint8_t(0b11111))) {
    return false;
  }
  uint32_t float_count = 0;
  for (uint64_t bitmap : analysis.constant_register_map.float_bitmap) {
    float_count += rex::bit_count(bitmap);
  }
  if (analysis.constant_register_map.float_dynamic_addressing) {
    if (float_count != 256 || analysis.constant_register_map.float_count != 256) {
      return false;
    }
  } else if (float_count != analysis.constant_register_map.float_count) {
    return false;
  }
  uint32_t vertex_fetch_seen[3] = {};
  for (const RuntimeAnalysis::VertexBinding& binding : analysis.vertex_bindings) {
    if (shader_type_ != xenos::ShaderType::kVertex || binding.fetch_constant >= 96 ||
        binding.stride_words > 0xFF ||
        (vertex_fetch_seen[binding.fetch_constant / 32] &
         (uint32_t(1) << (binding.fetch_constant % 32)))) {
      return false;
    }
    vertex_fetch_seen[binding.fetch_constant / 32] |=
        uint32_t(1) << (binding.fetch_constant % 32);
  }
  for (uint32_t constant : analysis.memexport_stream_constants) {
    if (constant >= 256) {
      return false;
    }
  }

  vertex_bindings_.reserve(analysis.vertex_bindings.size());
  for (const RuntimeAnalysis::VertexBinding& binding : analysis.vertex_bindings) {
    vertex_bindings_.push_back({int(vertex_bindings_.size()), binding.fetch_constant,
                                binding.stride_words, {}});
  }
  constant_register_map_ = analysis.constant_register_map;
  memexport_stream_constants_ = std::move(analysis.memexport_stream_constants);
  register_static_address_bound_ = analysis.register_static_address_bound;
  writes_interpolators_ = analysis.writes_interpolators;
  writes_point_size_edge_flag_kill_vertex_ =
      analysis.writes_point_size_edge_flag_kill_vertex;
  writes_color_targets_ = analysis.writes_color_targets;
  memexport_eM_written_ = analysis.memexport_eM_written;
  uses_register_dynamic_addressing_ = analysis.uses_register_dynamic_addressing;
  kills_pixels_ = analysis.kills_pixels;
  uses_texture_fetch_instruction_results_ = analysis.uses_texture_fetch_instruction_results;
  writes_depth_ = analysis.writes_depth;
  is_ucode_analyzed_ = true;
  return true;
}

std::string Shader::Translation::GetTranslatedBinaryString() const {
  std::string result;
  result.resize(translated_binary_.size());
  std::memcpy(const_cast<char*>(result.data()), translated_binary_.data(),
              translated_binary_.size());
  return result;
}

std::pair<std::filesystem::path, std::filesystem::path> Shader::Translation::Dump(
    const std::filesystem::path& base_path, const char* path_prefix) const {
  if (!is_valid()) {
    return std::make_pair(std::filesystem::path(), std::filesystem::path());
  }

  std::filesystem::path path = base_path;
  // Ensure target path exists.
  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    target_path = std::filesystem::absolute(target_path);
    std::filesystem::create_directories(target_path);
  }

  const char* type_extension = shader().type() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / fmt::format("shader_{:016X}_{:016X}.{}.bin.{}", shader().ucode_data_hash(),
                                modification(), path_prefix, type_extension);
  FILE* binary_file = filesystem::OpenFile(binary_path, "wb");
  if (binary_file) {
    fwrite(translated_binary_.data(), sizeof(*translated_binary_.data()), translated_binary_.size(),
           binary_file);
    fclose(binary_file);
  }

  std::filesystem::path disasm_path;
  if (!host_disassembly_.empty()) {
    disasm_path =
        target_path / fmt::format("shader_{:016X}_{:016X}.{}.{}", shader().ucode_data_hash(),
                                  modification(), path_prefix, type_extension);
    FILE* disasm_file = filesystem::OpenFile(disasm_path, "w");
    if (disasm_file) {
      fwrite(host_disassembly_.data(), sizeof(*host_disassembly_.data()), host_disassembly_.size(),
             disasm_file);
      fclose(disasm_file);
    }
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

Shader::Translation* Shader::GetOrCreateTranslation(uint64_t modification, bool* is_new) {
  auto it = translations_.find(modification);
  if (it != translations_.end()) {
    if (is_new) {
      *is_new = false;
    }
    return it->second;
  }
  Translation* translation = CreateTranslationInstance(modification);
  translations_.emplace(modification, translation);
  if (is_new) {
    *is_new = true;
  }
  return translation;
}

void Shader::DestroyTranslation(uint64_t modification) {
  auto it = translations_.find(modification);
  if (it == translations_.end()) {
    return;
  }
  delete it->second;
  translations_.erase(it);
}

std::pair<std::filesystem::path, std::filesystem::path> Shader::DumpUcode(
    const std::filesystem::path& base_path) const {
  // Ensure target path exists.
  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    target_path = std::filesystem::absolute(target_path);
    std::filesystem::create_directories(target_path);
  }

  const char* type_extension = type() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / fmt::format("shader_{:016X}.ucode.bin.{}", ucode_data_hash(), type_extension);
  FILE* binary_file = filesystem::OpenFile(binary_path, "wb");
  if (binary_file) {
    fwrite(ucode_data().data(), sizeof(*ucode_data().data()), ucode_data().size(), binary_file);
    fclose(binary_file);
  }

  std::filesystem::path disasm_path;
  if (is_ucode_analyzed()) {
    disasm_path =
        target_path / fmt::format("shader_{:016X}.ucode.{}", ucode_data_hash(), type_extension);
    FILE* disasm_file = filesystem::OpenFile(disasm_path, "w");
    if (disasm_file) {
      fwrite(ucode_disassembly().data(), sizeof(*ucode_disassembly().data()),
             ucode_disassembly().size(), disasm_file);
      fclose(disasm_file);
    }
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

Shader::Translation* Shader::CreateTranslationInstance(uint64_t modification) {
  // Default implementation for simple cases like ucode disassembly.
  return new Translation(*this, modification);
}

}  // namespace rex::graphics
