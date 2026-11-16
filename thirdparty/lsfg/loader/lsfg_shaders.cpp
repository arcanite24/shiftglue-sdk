// The resource walk is ours (as in ShiftGlue's LSFG census); the name table
// and the binding renumbering follow lsfg-vk 1.0.0 src/extract (MIT, see
// ../README.md).

#include "lsfg_shaders.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

#include <dxbc/dxbc_modinfo.h>
#include <dxbc/dxbc_module.h>
#include <dxbc/dxbc_reader.h>

namespace lsfg {

namespace {

const std::unordered_map<std::string, uint32_t>& NameTable() {
  static const std::unordered_map<std::string, uint32_t> table = {
      {"mipmaps", 255},     {"alpha[0]", 267},    {"alpha[1]", 268},    {"alpha[2]", 269},
      {"alpha[3]", 270},    {"beta[0]", 275},     {"beta[1]", 276},     {"beta[2]", 277},
      {"beta[3]", 278},     {"beta[4]", 279},     {"gamma[0]", 257},    {"gamma[1]", 259},
      {"gamma[2]", 260},    {"gamma[3]", 261},    {"gamma[4]", 262},    {"delta[0]", 257},
      {"delta[1]", 263},    {"delta[2]", 264},    {"delta[3]", 265},    {"delta[4]", 266},
      {"delta[5]", 258},    {"delta[6]", 271},    {"delta[7]", 272},    {"delta[8]", 273},
      {"delta[9]", 274},    {"generate", 256},    {"p_mipmaps", 255},   {"p_alpha[0]", 290},
      {"p_alpha[1]", 291},  {"p_alpha[2]", 292},  {"p_alpha[3]", 293},  {"p_beta[0]", 298},
      {"p_beta[1]", 299},   {"p_beta[2]", 300},   {"p_beta[3]", 301},   {"p_beta[4]", 302},
      {"p_gamma[0]", 280},  {"p_gamma[1]", 282},  {"p_gamma[2]", 283},  {"p_gamma[3]", 284},
      {"p_gamma[4]", 285},  {"p_delta[0]", 280},  {"p_delta[1]", 286},  {"p_delta[2]", 287},
      {"p_delta[3]", 288},  {"p_delta[4]", 289},  {"p_delta[5]", 281},  {"p_delta[6]", 294},
      {"p_delta[7]", 295},  {"p_delta[8]", 296},  {"p_delta[9]", 297},  {"p_generate", 256},
  };
  return table;
}

template <typename T>
bool ReadAt(const std::vector<uint8_t>& data, size_t offset, T* value) {
  if (offset > data.size() || data.size() - offset < sizeof(T)) {
    return false;
  }
  std::memcpy(value, data.data() + offset, sizeof(T));
  return true;
}

}  // namespace

std::map<uint32_t, std::vector<uint8_t>> ReadRcdataResources(const std::filesystem::path& path) {
  std::map<uint32_t, std::vector<uint8_t>> resources;
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  uint16_t mz = 0;
  uint32_t pe_offset = 0;
  uint32_t signature = 0;
  if (!ReadAt(data, 0, &mz) || mz != 0x5A4D || !ReadAt(data, 0x3C, &pe_offset) ||
      !ReadAt(data, pe_offset, &signature) || signature != 0x00004550) {
    return resources;
  }
  const size_t file_header = size_t(pe_offset) + 4;
  const size_t optional = file_header + 20;
  uint16_t section_count = 0;
  uint16_t optional_size = 0;
  uint16_t magic = 0;
  if (!ReadAt(data, file_header + 2, &section_count) ||
      !ReadAt(data, file_header + 16, &optional_size) || !ReadAt(data, optional, &magic)) {
    return resources;
  }
  const size_t directories = optional + (magic == 0x20B ? 112 : 96);
  uint32_t resource_rva = 0;
  if (!ReadAt(data, directories + 2 * 8, &resource_rva) || !resource_rva) {
    return resources;
  }
  struct Section {
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_size = 0;
    uint32_t raw_offset = 0;
  };
  std::vector<Section> sections;
  const size_t section_table = optional + optional_size;
  for (uint16_t i = 0; i < section_count; ++i) {
    const size_t entry = section_table + size_t(i) * 40;
    Section section;
    if (!ReadAt(data, entry + 8, &section.virtual_size) ||
        !ReadAt(data, entry + 12, &section.virtual_address) ||
        !ReadAt(data, entry + 16, &section.raw_size) ||
        !ReadAt(data, entry + 20, &section.raw_offset)) {
      return resources;
    }
    sections.push_back(section);
  }
  auto to_offset = [&sections](uint32_t rva, size_t* offset) {
    for (const Section& section : sections) {
      const uint32_t size = std::max(section.virtual_size, section.raw_size);
      if (rva >= section.virtual_address && rva - section.virtual_address < size) {
        *offset = size_t(section.raw_offset) + (rva - section.virtual_address);
        return true;
      }
    }
    return false;
  };
  size_t base = 0;
  if (!to_offset(resource_rva, &base)) {
    return resources;
  }
  // Level 1 is the type (RT_RCDATA = 10), level 2 the name (a numeric id),
  // level 3 the language.
  std::function<void(size_t, int, uint32_t, uint32_t)> walk = [&](size_t directory, int depth,
                                                                  uint32_t type, uint32_t id) {
    uint16_t named = 0;
    uint16_t numbered = 0;
    if (depth > 3 || !ReadAt(data, directory + 12, &named) ||
        !ReadAt(data, directory + 14, &numbered)) {
      return;
    }
    for (uint32_t i = 0; i < uint32_t(named) + numbered; ++i) {
      const size_t entry = directory + 16 + size_t(i) * 8;
      uint32_t name = 0;
      uint32_t target = 0;
      if (!ReadAt(data, entry, &name) || !ReadAt(data, entry + 4, &target)) {
        return;
      }
      const uint32_t value = (name & 0x80000000u) ? 0 : name;
      const uint32_t next_type = depth == 1 ? value : type;
      const uint32_t next_id = depth == 2 ? value : id;
      if (depth == 1 && next_type != 10) {
        continue;
      }
      if (target & 0x80000000u) {
        walk(base + (target & 0x7FFFFFFFu), depth + 1, next_type, next_id);
        continue;
      }
      uint32_t data_rva = 0;
      uint32_t data_size = 0;
      size_t data_offset = 0;
      if (next_type == 10 && next_id && ReadAt(data, base + target, &data_rva) &&
          ReadAt(data, base + target + 4, &data_size) && to_offset(data_rva, &data_offset) &&
          data_offset <= data.size() && data.size() - data_offset >= data_size) {
        resources[next_id].assign(data.begin() + data_offset,
                                  data.begin() + data_offset + data_size);
      }
    }
  };
  walk(base, 1, 0, 0);
  return resources;
}

uint32_t ShaderResourceId(const std::string& name) {
  const auto it = NameTable().find(name);
  return it == NameTable().end() ? 0 : it->second;
}

std::vector<std::string> ShaderNames() {
  std::vector<std::string> names;
  for (const auto& entry : NameTable()) {
    names.push_back(entry.first);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<uint8_t> TranslateShader(const std::vector<uint8_t>& dxbc) {
  dxvk::DxbcReader reader(reinterpret_cast<const char*>(dxbc.data()), dxbc.size());
  dxvk::DxbcModule module(reader);
  const dxvk::DxbcModuleInfo info{};
  dxvk::SpirvCodeBuffer code = module.compile(info, "CS");

  // Renumber the bindings 0..N-1 in the order they are decorated (lsfg-vk
  // 1.0.0 src/extract/trans.cpp).
  struct Binding {
    uint32_t binding_offset = 0;
  };
  std::vector<Binding> bindings;
  std::vector<uint32_t> variables;
  for (auto instruction : code) {
    if (instruction.opCode() == spv::OpDecorate &&
        instruction.arg(2) == spv::DecorationBinding) {
      const uint32_t variable = instruction.arg(1);
      bindings.resize(std::max(bindings.size(), size_t(variable) + 1));
      bindings[variable].binding_offset = instruction.offset() + 3;
      variables.push_back(variable);
    }
    if (instruction.opCode() == spv::OpFunction) {
      break;
    }
  }
  uint32_t* words = code.data();
  uint32_t index = 0;
  for (const uint32_t variable : variables) {
    if (bindings[variable].binding_offset) {
      words[bindings[variable].binding_offset] = index++;
    }
  }
  std::vector<uint8_t> spirv(code.size());
  std::memcpy(spirv.data(), code.data(), code.size());
  return spirv;
}

}  // namespace lsfg
