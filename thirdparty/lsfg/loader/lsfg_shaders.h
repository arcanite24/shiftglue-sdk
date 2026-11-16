#pragma once

// LSFG shaders from the player's own Lossless.dll: the DXBC resources are
// read at run time and translated to SPIR-V in memory. Nothing is written
// to disk. Names and resource ids are lsfg-vk 1.0.0's (src/extract).

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lsfg {

// The RCDATA resources of a PE file by numeric id. Empty when the file is
// not a PE file with resources.
std::map<uint32_t, std::vector<uint8_t>> ReadRcdataResources(const std::filesystem::path& path);

// The resource id of a framegen shader name ("alpha[0]", "p_generate", ...),
// or 0 for an unknown name.
uint32_t ShaderResourceId(const std::string& name);

// Every shader name LSFG 3.1 and its performance mode use.
std::vector<std::string> ShaderNames();

// DXBC compute shader to SPIR-V, with its bindings renumbered 0..N-1 in
// declaration order, as framegen's descriptor set layouts expect. Throws on
// a shader the compiler cannot read.
std::vector<uint8_t> TranslateShader(const std::vector<uint8_t>& dxbc);

}  // namespace lsfg
