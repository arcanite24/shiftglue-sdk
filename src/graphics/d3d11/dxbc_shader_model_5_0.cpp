/**
 * @file        graphics/d3d11/dxbc_shader_model_5_0.cpp
 * @brief       Shader model 5.1 to 5.0 DXBC conversion for built-in shaders.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/dxbc_shader_model_5_0.h>

#include <cstring>
#include <string>

#include "thirdparty/dxbc/DXBCChecksum.h"

namespace rex::graphics::d3d11 {

namespace {

constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
         (uint32_t(uint8_t(d)) << 24);
}
constexpr uint32_t kFourCCContainer = MakeFourCC('D', 'X', 'B', 'C');
constexpr uint32_t kFourCCResourceDefinition = MakeFourCC('R', 'D', 'E', 'F');
constexpr uint32_t kFourCCShaderEx = MakeFourCC('S', 'H', 'E', 'X');

// Opcodes (D3D10_SB_OPCODE_TYPE / D3D11_SB_OPCODE_TYPE).
enum : uint32_t {
  kOpcodeCustomData = 53,
  kOpcodeDclResource = 88,
  kOpcodeDclConstantBuffer = 89,
  kOpcodeDclSampler = 90,
  kOpcodeDclGlobalFlags = 106,
  kOpcodeDclStream = 143,
  kOpcodeDclUnorderedAccessViewTyped = 156,
  kOpcodeDclUnorderedAccessViewRaw = 157,
  kOpcodeDclUnorderedAccessViewStructured = 158,
  kOpcodeDclResourceRaw = 161,
  kOpcodeDclResourceStructured = 162,
};

// Operand types.
enum : uint32_t {
  kOperandImmediate32 = 4,
  kOperandImmediate64 = 5,
  kOperandSampler = 6,
  kOperandResource = 7,
  kOperandConstantBuffer = 8,
  kOperandUnorderedAccessView = 30,
};

// Operand index representations.
enum : uint32_t {
  kIndexImmediate32 = 0,
  kIndexImmediate64 = 1,
  kIndexRelative = 2,
  kIndexImmediate32PlusRelative = 3,
  kIndexImmediate64PlusRelative = 4,
};

bool IsDeclaration(uint32_t opcode) {
  return (opcode >= kOpcodeDclResource && opcode <= kOpcodeDclGlobalFlags) ||
         (opcode >= kOpcodeDclStream && opcode <= kOpcodeDclResourceStructured);
}

bool IsBindingOperandType(uint32_t type) {
  return type == kOperandSampler || type == kOperandResource || type == kOperandConstantBuffer ||
         type == kOperandUnorderedAccessView;
}

class Converter {
 public:
  explicit Converter(std::string& error) : error_(error) {}

  bool Fail(const std::string& message) {
    if (error_.empty()) {
      error_ = message;
    }
    return false;
  }

  // Converts one operand (with its extended tokens, immediate values and
  // relative index operands) of an instruction.
  bool ConvertOperand(const uint32_t*& in, const uint32_t* end, std::vector<uint32_t>& out) {
    if (in >= end) {
      return Fail("operand past the end of the instruction");
    }
    uint32_t token = *(in++);
    uint32_t type = (token >> 12) & 0xFF;
    uint32_t dimension = (token >> 20) & 3;
    std::vector<uint32_t> extended;
    bool has_extended = (token >> 31) != 0;
    while (has_extended) {
      if (in >= end) {
        return Fail("extended operand token past the end of the instruction");
      }
      uint32_t extended_token = *(in++);
      extended.push_back(extended_token);
      has_extended = (extended_token >> 31) != 0;
    }

    if (type == kOperandImmediate32 || type == kOperandImmediate64) {
      uint32_t component_count_mode = token & 3;
      uint32_t components = component_count_mode == 1 ? 1 : (component_count_mode == 2 ? 4 : 0);
      uint32_t dwords = components * (type == kOperandImmediate64 ? 2 : 1);
      if (size_t(end - in) < dwords) {
        return Fail("immediate operand past the end of the instruction");
      }
      out.push_back(token);
      out.insert(out.end(), extended.begin(), extended.end());
      out.insert(out.end(), in, in + dwords);
      in += dwords;
      return true;
    }

    struct Index {
      uint32_t representation;
      std::vector<uint32_t> dwords;
    };
    Index indices[3];
    for (uint32_t i = 0; i < dimension; ++i) {
      uint32_t representation = (token >> (22 + 3 * i)) & 7;
      indices[i].representation = representation;
      uint32_t immediate_dwords = 0;
      bool relative = false;
      switch (representation) {
        case kIndexImmediate32:
          immediate_dwords = 1;
          break;
        case kIndexImmediate64:
          immediate_dwords = 2;
          break;
        case kIndexRelative:
          relative = true;
          break;
        case kIndexImmediate32PlusRelative:
          immediate_dwords = 1;
          relative = true;
          break;
        case kIndexImmediate64PlusRelative:
          immediate_dwords = 2;
          relative = true;
          break;
        default:
          return Fail("unknown operand index representation");
      }
      if (size_t(end - in) < immediate_dwords) {
        return Fail("operand index past the end of the instruction");
      }
      indices[i].dwords.insert(indices[i].dwords.end(), in, in + immediate_dwords);
      in += immediate_dwords;
      if (relative && !ConvertOperand(in, end, indices[i].dwords)) {
        return false;
      }
    }

    uint32_t first_kept_index = 0;
    if (IsBindingOperandType(type)) {
      // Shader model 5.1: [ID][register] ([ID][register][element] for
      // constant buffers). Shader model 5.0: [register] ([register][element]).
      uint32_t expected_dimension = type == kOperandConstantBuffer ? 3 : 2;
      if (dimension != expected_dimension) {
        return Fail("binding operand without the shader model 5.1 [ID][register] form");
      }
      if (indices[0].representation != kIndexImmediate32 ||
          indices[1].representation != kIndexImmediate32) {
        return Fail("binding operand with a dynamic range ID or register index");
      }
      first_kept_index = 1;
    }

    uint32_t new_dimension = dimension - first_kept_index;
    uint32_t new_token = token & ~((uint32_t(3) << 20) | (uint32_t(0x1FF) << 22));
    new_token |= new_dimension << 20;
    for (uint32_t i = 0; i < new_dimension; ++i) {
      new_token |= indices[first_kept_index + i].representation << (22 + 3 * i);
    }
    out.push_back(new_token);
    out.insert(out.end(), extended.begin(), extended.end());
    for (uint32_t i = first_kept_index; i < dimension; ++i) {
      out.insert(out.end(), indices[i].dwords.begin(), indices[i].dwords.end());
    }
    return true;
  }

  // Resource declarations: operand [ID][lower][upper], then per-opcode
  // dwords, then the register space.
  bool ConvertBindingDeclaration(uint32_t opcode_token, const uint32_t* in, const uint32_t* end,
                                 std::vector<uint32_t>& out) {
    uint32_t opcode = opcode_token & 0x7FF;
    if (end - in < 4) {
      return Fail("truncated resource declaration");
    }
    uint32_t operand_token = in[0];
    uint32_t type = (operand_token >> 12) & 0xFF;
    uint32_t dimension = (operand_token >> 20) & 3;
    if ((operand_token >> 31) != 0 || !IsBindingOperandType(type) || dimension != 3 ||
        ((operand_token >> 22) & 0x1FF) != 0) {
      return Fail("resource declaration without an immediate [ID][lower][upper] operand");
    }
    uint32_t lower = in[2];
    uint32_t upper = in[3];
    if (lower != upper) {
      return Fail("resource declaration of a register range (array)");
    }
    const uint32_t* tail = in + 4;
    size_t tail_dwords = size_t(end - tail);
    // Dwords between the operand and the space.
    size_t middle_dwords;
    switch (opcode) {
      case kOpcodeDclResource:
      case kOpcodeDclConstantBuffer:
      case kOpcodeDclUnorderedAccessViewTyped:
      case kOpcodeDclUnorderedAccessViewStructured:
      case kOpcodeDclResourceStructured:
        middle_dwords = 1;
        break;
      default:
        middle_dwords = 0;
        break;
    }
    if (tail_dwords != middle_dwords + 1) {
      return Fail("unexpected resource declaration length");
    }
    if (tail[middle_dwords] != 0) {
      return Fail("resource declaration in a register space other than 0");
    }

    std::vector<uint32_t> body;
    uint32_t new_operand_token = operand_token & ~((uint32_t(3) << 20) | (uint32_t(0x1FF) << 22));
    if (opcode == kOpcodeDclConstantBuffer) {
      // cb#[size in vectors].
      body.push_back(new_operand_token | (uint32_t(2) << 20));
      body.push_back(lower);
      body.push_back(tail[0]);
    } else {
      // fxc declares t#, s# and u# as 0-component operands in shader model
      // 5.0 (5.1 declares them with a 4-component swizzle).
      body.push_back((new_operand_token & ~uint32_t(0xFFF)) | (uint32_t(1) << 20));
      body.push_back(lower);
      // Return type or structure stride.
      body.insert(body.end(), tail, tail + middle_dwords);
    }
    uint32_t length = 1 + uint32_t(body.size());
    out.push_back((opcode_token & ~(uint32_t(0x7F) << 24)) | (length << 24));
    out.insert(out.end(), body.begin(), body.end());
    return true;
  }

  bool ConvertProgram(const uint32_t* program, size_t program_dwords, std::vector<uint32_t>& out) {
    if (program_dwords < 2 || program[1] != program_dwords) {
      return Fail("bad shader program length");
    }
    uint32_t version = program[0];
    if ((version & 0xFF) != 0x51) {
      return Fail("not a shader model 5.1 program");
    }
    size_t program_start = out.size();
    out.push_back((version & ~uint32_t(0xF)) | 0x0);
    out.push_back(0);
    const uint32_t* in = program + 2;
    const uint32_t* program_end = program + program_dwords;
    while (in < program_end) {
      uint32_t opcode_token = in[0];
      uint32_t opcode = opcode_token & 0x7FF;
      uint32_t length;
      if (opcode == kOpcodeCustomData) {
        if (program_end - in < 2) {
          return Fail("truncated custom data block");
        }
        length = in[1];
      } else {
        length = (opcode_token >> 24) & 0x7F;
      }
      if (!length || length > size_t(program_end - in)) {
        return Fail("bad instruction length");
      }
      const uint32_t* instruction_end = in + length;
      if (opcode == kOpcodeCustomData) {
        out.insert(out.end(), in, instruction_end);
      } else if (opcode == kOpcodeDclResource || opcode == kOpcodeDclConstantBuffer ||
                 opcode == kOpcodeDclSampler || opcode == kOpcodeDclUnorderedAccessViewTyped ||
                 opcode == kOpcodeDclUnorderedAccessViewRaw ||
                 opcode == kOpcodeDclUnorderedAccessViewStructured ||
                 opcode == kOpcodeDclResourceRaw || opcode == kOpcodeDclResourceStructured) {
        if ((opcode_token >> 31) != 0) {
          return Fail("extended resource declaration");
        }
        if (!ConvertBindingDeclaration(opcode_token, in + 1, instruction_end, out)) {
          return false;
        }
      } else if (IsDeclaration(opcode)) {
        // No binding operands in the other declarations.
        out.insert(out.end(), in, instruction_end);
      } else {
        size_t instruction_start = out.size();
        const uint32_t* operand = in + 1;
        out.push_back(opcode_token);
        bool has_extended = (opcode_token >> 31) != 0;
        while (has_extended) {
          if (operand >= instruction_end) {
            return Fail("extended opcode token past the end of the instruction");
          }
          uint32_t extended_token = *(operand++);
          out.push_back(extended_token);
          has_extended = (extended_token >> 31) != 0;
        }
        while (operand < instruction_end) {
          if (!ConvertOperand(operand, instruction_end, out)) {
            return false;
          }
        }
        size_t new_length = out.size() - instruction_start;
        if (new_length > 0x7F) {
          return Fail("converted instruction too long");
        }
        out[instruction_start] =
            (opcode_token & ~(uint32_t(0x7F) << 24)) | (uint32_t(new_length) << 24);
      }
      in = instruction_end;
    }
    out[program_start + 1] = uint32_t(out.size() - program_start);
    return true;
  }

 private:
  std::string& error_;
};

}  // namespace

bool ConvertDxbcToShaderModel5_0(const void* dxbc, size_t dxbc_size,
                                 std::vector<uint32_t>& container_out, std::string& error_out) {
  error_out.clear();
  container_out.clear();
  Converter converter(error_out);
  if (dxbc_size < 32 || (dxbc_size & 3) || (reinterpret_cast<uintptr_t>(dxbc) & 3)) {
    return converter.Fail("bad DXBC container size or alignment");
  }
  const uint32_t* container = static_cast<const uint32_t*>(dxbc);
  if (container[0] != kFourCCContainer || container[6] != dxbc_size) {
    return converter.Fail("not a DXBC container");
  }
  uint32_t blob_count = container[7];
  if (32 + size_t(blob_count) * 4 > dxbc_size) {
    return converter.Fail("bad DXBC blob count");
  }

  struct Blob {
    uint32_t fourcc;
    std::vector<uint32_t> data;
  };
  std::vector<Blob> blobs;
  bool shader_converted = false;
  for (uint32_t i = 0; i < blob_count; ++i) {
    uint32_t offset = container[8 + i];
    if ((offset & 3) || size_t(offset) + 8 > dxbc_size) {
      return converter.Fail("bad DXBC blob offset");
    }
    const uint32_t* blob_header = container + offset / 4;
    uint32_t fourcc = blob_header[0];
    uint32_t size = blob_header[1];
    if ((size & 3) || size_t(offset) + 8 + size > dxbc_size) {
      return converter.Fail("bad DXBC blob size");
    }
    if (fourcc == kFourCCResourceDefinition) {
      continue;
    }
    Blob blob;
    blob.fourcc = fourcc;
    if (fourcc == kFourCCShaderEx) {
      if (shader_converted) {
        return converter.Fail("more than one shader program");
      }
      if (!converter.ConvertProgram(blob_header + 2, size / 4, blob.data)) {
        return false;
      }
      shader_converted = true;
    } else {
      blob.data.assign(blob_header + 2, blob_header + 2 + size / 4);
    }
    blobs.push_back(std::move(blob));
  }
  if (!shader_converted) {
    return converter.Fail("no SHEX shader program");
  }

  // Header: fourcc, hash[4], version 1.0, size, blob count; blob offsets.
  container_out.resize(8 + blobs.size());
  container_out[0] = kFourCCContainer;
  container_out[5] = container[5];
  container_out[7] = uint32_t(blobs.size());
  for (size_t i = 0; i < blobs.size(); ++i) {
    container_out[8 + i] = uint32_t(container_out.size() * 4);
    container_out.push_back(blobs[i].fourcc);
    container_out.push_back(uint32_t(blobs[i].data.size() * 4));
    container_out.insert(container_out.end(), blobs[i].data.begin(), blobs[i].data.end());
  }
  uint32_t size_bytes = uint32_t(container_out.size() * 4);
  container_out[6] = size_bytes;
  CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(container_out.data()), size_bytes,
                        reinterpret_cast<unsigned int*>(&container_out[1]));
  return true;
}

}  // namespace rex::graphics::d3d11
