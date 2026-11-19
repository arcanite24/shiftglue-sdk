/**
 * @file        graphics/d3d11/dxbc_shader_model_5_0.h
 * @brief       Shader model 5.1 to 5.0 DXBC conversion for built-in shaders.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rex::graphics::d3d11 {

// Converts a shader model 5.1 DXBC container (the prebuilt Direct3D 12
// built-in shaders) to shader model 5.0 for feature level 11_0, so both
// backends run the same compiled code.
//
// Shader model 5.1 differs from 5.0 in the operand forms of resources only:
// declarations carry a range ID, the register range and a register space, and
// references are [ID][register] (constant buffers [ID][register][element]).
// The conversion keeps the register (never the ID) and drops the rest. It
// supports what the built-in shaders use and fails with a message otherwise:
// register space 0, single-register ranges, immediate register indices.
//
// The resource definition (RDEF) chunk, which has a shader model 5.1 layout,
// is dropped: the runtime doesn't need reflection to create a shader. The
// container checksum is recalculated.
bool ConvertDxbcToShaderModel5_0(const void* dxbc, size_t dxbc_size,
                                 std::vector<uint32_t>& container_out, std::string& error_out);

}  // namespace rex::graphics::d3d11
