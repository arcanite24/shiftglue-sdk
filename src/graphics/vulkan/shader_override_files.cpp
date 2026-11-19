#include <rex/graphics/vulkan/shader_override_files.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <optional>
#include <tuple>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <spirv-tools/libspirv.h>

namespace rex::graphics::vulkan {
namespace {

constexpr size_t kMaximumBytes = 16 * 1024 * 1024;

// SPIR-V opcodes, decorations and storage classes read here.
enum : uint32_t {
  kOpEntryPoint = 15,
  kOpTypeBool = 20,
  kOpTypeInt = 21,
  kOpTypeFloat = 22,
  kOpTypeVector = 23,
  kOpTypeMatrix = 24,
  kOpTypeImage = 25,
  kOpTypeSampler = 26,
  kOpTypeSampledImage = 27,
  kOpTypeArray = 28,
  kOpTypeRuntimeArray = 29,
  kOpTypeStruct = 30,
  kOpTypePointer = 32,
  kOpConstant = 43,
  kOpVariable = 59,
  kOpDecorate = 71,
  kOpMemberDecorate = 72,
};
enum : uint32_t {
  kDecorationBlock = 2,
  kDecorationBufferBlock = 3,
  kDecorationArrayStride = 6,
  kDecorationBuiltIn = 11,
  kDecorationLocation = 30,
  kDecorationComponent = 31,
  kDecorationBinding = 33,
  kDecorationDescriptorSet = 34,
  kDecorationOffset = 35,
};
enum : uint32_t {
  kStorageUniformConstant = 0,
  kStorageInput = 1,
  kStorageUniform = 2,
  kStorageOutput = 3,
  kStoragePushConstant = 9,
  kStorageStorageBuffer = 12,
};

std::optional<uint32_t> MainExecutionModel(std::span<const uint32_t> words) {
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    if (!count || count > words.size() - i)
      return std::nullopt;
    if ((words[i] & 0xFFFF) == kOpEntryPoint && count >= 5 && words[i + 3] == 0x6E69616D &&
        words[i + 4] == 0)
      return words[i + 1];  // OpEntryPoint ... "main"
    i += count;
  }
  return std::nullopt;
}

// The module's types, constants, decorations and variables.
struct Module {
  // Result id -> the defining instruction's operands (without the opcode
  // word), for types and constants.
  std::unordered_map<uint32_t, std::pair<uint32_t, std::vector<uint32_t>>> types;
  std::unordered_map<uint32_t, uint32_t> constants;
  // (id, decoration) -> first literal (or 1 for decorations without one).
  std::unordered_map<uint64_t, uint32_t> decorations;
  // (struct id, member) -> decoration -> first literal.
  std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>> member_decorations;
  struct Variable {
    uint32_t pointer_type;
    uint32_t id;
    uint32_t storage;
  };
  std::vector<Variable> variables;

  static uint64_t Key(uint32_t id, uint32_t value) { return (uint64_t(id) << 32) | value; }
  std::optional<uint32_t> Decoration(uint32_t id, uint32_t decoration) const {
    const auto it = decorations.find(Key(id, decoration));
    if (it == decorations.end())
      return std::nullopt;
    return it->second;
  }
  std::optional<uint32_t> MemberDecoration(uint32_t id, uint32_t member,
                                           uint32_t decoration) const {
    const auto it = member_decorations.find(Key(id, member));
    if (it == member_decorations.end())
      return std::nullopt;
    const auto found = it->second.find(decoration);
    if (found == it->second.end())
      return std::nullopt;
    return found->second;
  }
  const std::pair<uint32_t, std::vector<uint32_t>>* Type(uint32_t id) const {
    const auto it = types.find(id);
    return it == types.end() ? nullptr : &it->second;
  }

  // The pointee of a pointer type, or the type itself.
  uint32_t Pointee(uint32_t id) const {
    const auto* type = Type(id);
    if (type && type->first == kOpTypePointer && type->second.size() >= 2)
      return type->second[1];
    return id;
  }

  std::string Describe(uint32_t id, int depth = 0) const {
    const auto* type = Type(id);
    if (!type || depth > 16)
      return "?";
    const auto& o = type->second;
    switch (type->first) {
      case kOpTypeBool:
        return "bool";
      case kOpTypeInt:
        return o.size() >= 2 ? fmt::format("{}{}", o[1] ? "i" : "u", o[0]) : "?";
      case kOpTypeFloat:
        return o.empty() ? "?" : fmt::format("f{}", o[0]);
      case kOpTypeVector:
        return o.size() >= 2 ? fmt::format("{}x{}", Describe(o[0], depth + 1), o[1]) : "?";
      case kOpTypeMatrix:
        return o.size() >= 2 ? fmt::format("{}m{}", Describe(o[0], depth + 1), o[1]) : "?";
      case kOpTypeArray: {
        if (o.size() < 2)
          return "?";
        const auto length = constants.find(o[1]);
        return fmt::format("{}[{}]", Describe(o[0], depth + 1),
                           length == constants.end() ? std::string("?")
                                                     : std::to_string(length->second));
      }
      case kOpTypeRuntimeArray:
        return o.empty() ? "?" : Describe(o[0], depth + 1) + "[]";
      case kOpTypeStruct: {
        std::string text = "{";
        for (size_t i = 0; i < o.size(); ++i)
          text += (i ? "," : "") + Describe(o[i], depth + 1);
        return text + "}";
      }
      case kOpTypeImage: {
        // Sampled type, Dim, Depth, Arrayed, MS, Sampled, Format. Depth is
        // a hint Vulkan ignores; the format matters only for storage images.
        if (o.size() < 7)
          return "?";
        std::string text = fmt::format("image {} dim{} arr{} ms{} sampled{}",
                                       Describe(o[0], depth + 1), o[1], o[3], o[4], o[5]);
        if (o[5] == 2)
          text += fmt::format(" format{}", o[6]);
        return text;
      }
      case kOpTypeSampler:
        return "sampler";
      case kOpTypeSampledImage:
        return o.empty() ? "?" : "sampled " + Describe(o[0], depth + 1);
      case kOpTypePointer:
        return o.size() < 2 ? "?" : Describe(o[1], depth + 1);
      default:
        return fmt::format("op{}", type->first);
    }
  }

  // Bytes from a block's start to the end of its last member.
  uint32_t Size(uint32_t id, int depth = 0) const {
    const auto* type = Type(id);
    if (!type || depth > 16)
      return 0;
    const auto& o = type->second;
    switch (type->first) {
      case kOpTypeBool:
        return 4;
      case kOpTypeInt:
      case kOpTypeFloat:
        return o.empty() ? 0 : o[0] / 8;
      case kOpTypeVector:
      case kOpTypeMatrix:
        return o.size() < 2 ? 0 : o[1] * Size(o[0], depth + 1);
      case kOpTypeArray: {
        if (o.size() < 2)
          return 0;
        const auto length = constants.find(o[1]);
        if (length == constants.end())
          return 0;
        const uint32_t stride =
            Decoration(id, kDecorationArrayStride).value_or(Size(o[0], depth + 1));
        return length->second * stride;
      }
      case kOpTypeStruct: {
        uint32_t end = 0, running = 0;
        for (uint32_t i = 0; i < o.size(); ++i) {
          const uint32_t offset = MemberDecoration(id, i, kDecorationOffset).value_or(running);
          running = offset + Size(o[i], depth + 1);
          end = std::max(end, running);
        }
        return end;
      }
      default:
        return 0;
    }
  }
};

bool Parse(std::span<const uint32_t> words, Module& module, uint32_t& execution_model,
           std::string& error) {
  if (words.size() < 5 || words[0] != 0x07230203u) {
    error = "not SPIR-V";
    return false;
  }
  const auto model = MainExecutionModel(words);
  if (!model) {
    error = "no main entry point";
    return false;
  }
  execution_model = *model;
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    const uint32_t opcode = words[i] & 0xFFFF;
    if (!count || count > words.size() - i) {
      error = "truncated instruction";
      return false;
    }
    std::span<const uint32_t> operands = words.subspan(i + 1, count - 1);
    switch (opcode) {
      case kOpTypeBool:
      case kOpTypeInt:
      case kOpTypeFloat:
      case kOpTypeVector:
      case kOpTypeMatrix:
      case kOpTypeImage:
      case kOpTypeSampler:
      case kOpTypeSampledImage:
      case kOpTypeArray:
      case kOpTypeRuntimeArray:
      case kOpTypeStruct:
      case kOpTypePointer:
        if (!operands.empty()) {
          module.types[operands[0]] = {
              opcode, std::vector<uint32_t>(operands.begin() + 1, operands.end())};
        }
        break;
      case kOpConstant:
        // Result type, result id, the value's low word.
        if (operands.size() >= 3)
          module.constants[operands[1]] = operands[2];
        break;
      case kOpVariable:
        if (operands.size() >= 3)
          module.variables.push_back({operands[0], operands[1], operands[2]});
        break;
      case kOpDecorate:
        if (operands.size() >= 2) {
          module.decorations[Module::Key(operands[0], operands[1])] =
              operands.size() >= 3 ? operands[2] : 1;
        }
        break;
      case kOpMemberDecorate:
        if (operands.size() >= 3) {
          module.member_decorations[Module::Key(operands[0], operands[1])][operands[2]] =
              operands.size() >= 4 ? operands[3] : 1;
        }
        break;
      default:
        break;
    }
    i += count;
  }
  return true;
}

bool SameVariable(const SpirvInterface::Variable& a, const SpirvInterface::Variable& b) {
  return a.location == b.location && a.component == b.component && a.type == b.type;
}

std::string VariableName(const SpirvInterface::Variable& variable) {
  return fmt::format("location {}.{} ({})", variable.location, variable.component, variable.type);
}

bool Validate(std::span<const uint8_t> bytes, std::span<const uint8_t> original,
              std::string& error) {
  if (bytes.size() < 20 || bytes.size() % 4 || bytes.size() > kMaximumBytes) {
    error = "invalid SPIR-V size";
    return false;
  }
  std::vector<uint32_t> words(bytes.size() / 4), original_words(original.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  std::memcpy(original_words.data(), original.data(), original_words.size() * 4);
  spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_2);
  if (!context) {
    error = "cannot create SPIR-V validator";
    return false;
  }
  spv_diagnostic diagnostic = nullptr;
  const auto result = spvValidateBinary(context, words.data(), words.size(), &diagnostic);
  if (result != SPV_SUCCESS)
    error = diagnostic ? diagnostic->error : "invalid SPIR-V";
  spvDiagnosticDestroy(diagnostic);
  spvContextDestroy(context);
  if (result != SPV_SUCCESS)
    return false;
  const auto model = MainExecutionModel(words);
  if (!model || model != MainExecutionModel(original_words)) {
    error = "main entry point does not match the translated stage";
    return false;
  }
  SpirvInterface replacement, translation;
  std::string reflect_error;
  if (!ReflectSpirvInterface(words, replacement, reflect_error)) {
    error = "cannot read its interface: " + reflect_error;
    return false;
  }
  if (!ReflectSpirvInterface(original_words, translation, reflect_error)) {
    error = "cannot read the translation's interface: " + reflect_error;
    return false;
  }
  std::string why;
  if (!SpirvInterfaceFits(replacement, translation, why)) {
    error = "interface does not fit the translation: " + why;
    return false;
  }
  return true;
}

}  // namespace

std::string VulkanShaderOverrideFilename(uint64_t guest, uint64_t modification,
                                         std::span<const uint8_t> original) {
  return fmt::format("shader_{:016X}_{:016X}_{:016X}.spv", guest, modification,
                     XXH3_64bits(original.data(), original.size()));
}

std::vector<std::string> VulkanShaderOverrideGuestFilenames(uint64_t guest, uint64_t modification) {
  return {fmt::format("shader_{:016X}_{:016X}.spv", guest, modification),
          fmt::format("shader_{:016X}.spv", guest)};
}

bool ReflectSpirvInterface(std::span<const uint32_t> words, SpirvInterface& out,
                           std::string& error) {
  out = {};
  Module module;
  if (!Parse(words, module, out.execution_model, error))
    return false;
  for (const Module::Variable& variable : module.variables) {
    const uint32_t pointee = module.Pointee(variable.pointer_type);
    switch (variable.storage) {
      case kStorageUniformConstant:
      case kStorageUniform:
      case kStorageStorageBuffer: {
        SpirvInterface::Resource resource;
        resource.set = module.Decoration(variable.id, kDecorationDescriptorSet).value_or(0);
        resource.binding = module.Decoration(variable.id, kDecorationBinding).value_or(0);
        uint32_t element = pointee;
        if (const auto* type = module.Type(pointee)) {
          if (type->first == kOpTypeArray && type->second.size() >= 2) {
            element = type->second[0];
            const auto length = module.constants.find(type->second[1]);
            resource.count = length == module.constants.end() ? ~0u : length->second;
          } else if (type->first == kOpTypeRuntimeArray && !type->second.empty()) {
            element = type->second[0];
            resource.count = ~0u;
          }
        }
        if (variable.storage == kStorageUniformConstant) {
          resource.kind = module.Describe(element);
        } else {
          const bool storage =
              variable.storage == kStorageStorageBuffer ||
              module.Decoration(element, kDecorationBufferBlock).has_value();
          resource.kind = storage ? "ssbo" : "ubo";
          resource.bytes = module.Size(element);
        }
        out.resources.push_back(std::move(resource));
        break;
      }
      case kStoragePushConstant:
        out.push_constant_bytes = std::max(out.push_constant_bytes, module.Size(pointee));
        break;
      case kStorageInput:
      case kStorageOutput: {
        const bool input = variable.storage == kStorageInput;
        auto& builtins = input ? out.builtin_inputs : out.builtin_outputs;
        if (const auto builtin = module.Decoration(variable.id, kDecorationBuiltIn)) {
          builtins.push_back(*builtin);
          break;
        }
        // A block of built-ins (gl_PerVertex), possibly arrayed per vertex.
        uint32_t block = pointee;
        if (const auto* type = module.Type(block);
            type && (type->first == kOpTypeArray || type->first == kOpTypeRuntimeArray) &&
            !type->second.empty()) {
          block = type->second[0];
        }
        bool member_builtins = false;
        if (const auto* type = module.Type(block); type && type->first == kOpTypeStruct) {
          for (uint32_t i = 0; i < type->second.size(); ++i) {
            if (const auto builtin = module.MemberDecoration(block, i, kDecorationBuiltIn)) {
              builtins.push_back(*builtin);
              member_builtins = true;
            }
          }
        }
        if (member_builtins)
          break;
        SpirvInterface::Variable io;
        io.location = module.Decoration(variable.id, kDecorationLocation).value_or(~0u);
        io.component = module.Decoration(variable.id, kDecorationComponent).value_or(0);
        io.type = module.Describe(pointee);
        (input ? out.inputs : out.outputs).push_back(std::move(io));
        break;
      }
      default:
        break;
    }
  }
  const auto order = [](const SpirvInterface::Variable& a, const SpirvInterface::Variable& b) {
    return std::tie(a.location, a.component) < std::tie(b.location, b.component);
  };
  std::sort(out.inputs.begin(), out.inputs.end(), order);
  std::sort(out.outputs.begin(), out.outputs.end(), order);
  for (auto* builtins : {&out.builtin_inputs, &out.builtin_outputs}) {
    std::sort(builtins->begin(), builtins->end());
    builtins->erase(std::unique(builtins->begin(), builtins->end()), builtins->end());
  }
  return true;
}

namespace {
std::atomic<bool> g_host_inputs{false};

// A host input the replacement may declare beyond the translation's layout.
bool IsHostInput(const SpirvInterface& replacement, const SpirvInterface::Resource& resource) {
  constexpr uint32_t kExecutionModelFragment = 4;
  if (!g_host_inputs.load(std::memory_order_relaxed) ||
      replacement.execution_model != kExecutionModelFragment || resource.set != 0 ||
      resource.count > 1) {
    return false;
  }
  if (resource.binding == kHostInputBlueNoiseBinding || resource.binding == kHostInputSceneAoBinding) {
    return resource.kind.rfind("sampled image", 0) == 0 &&
           resource.kind.find("dim1 arr0 ms0") != std::string::npos;
  }
  return resource.binding == kHostInputConstantsBinding && resource.kind == "ubo" &&
         resource.bytes <= sizeof(HostInputConstants);
}
}  // namespace

void SetVulkanShaderOverrideHostInputs(bool enabled) { g_host_inputs = enabled; }
bool VulkanShaderOverrideHostInputs() { return g_host_inputs; }

bool SpirvInterfaceFits(const SpirvInterface& replacement, const SpirvInterface& translation,
                        std::string& why) {
  if (replacement.execution_model != translation.execution_model) {
    why = fmt::format("stage {} instead of {}", replacement.execution_model,
                      translation.execution_model);
    return false;
  }
  for (const SpirvInterface::Resource& resource : replacement.resources) {
    const auto match = std::find_if(
        translation.resources.begin(), translation.resources.end(), [&](const auto& other) {
          return other.set == resource.set && other.binding == resource.binding;
        });
    if (match == translation.resources.end() && IsHostInput(replacement, resource)) {
      continue;
    }
    if (match == translation.resources.end()) {
      why = fmt::format("set {} binding {} ({}) is not in the translation's layout", resource.set,
                        resource.binding, resource.kind);
      return false;
    }
    if (match->kind != resource.kind) {
      why = fmt::format("set {} binding {} is {}, the translation's is {}", resource.set,
                        resource.binding, resource.kind, match->kind);
      return false;
    }
    const auto elements = [](uint32_t count) { return count ? count : 1u; };
    if (elements(resource.count) > elements(match->count)) {
      why = fmt::format("set {} binding {} has {} elements, the translation's {}", resource.set,
                        resource.binding, elements(resource.count), elements(match->count));
      return false;
    }
    if (resource.bytes > match->bytes) {
      why = fmt::format("set {} binding {} reads {} bytes, the translation's block has {}",
                        resource.set, resource.binding, resource.bytes, match->bytes);
      return false;
    }
  }
  if (replacement.push_constant_bytes > translation.push_constant_bytes) {
    why = fmt::format("{} bytes of push constants, the translation has {}",
                      replacement.push_constant_bytes, translation.push_constant_bytes);
    return false;
  }
  for (const SpirvInterface::Variable& input : replacement.inputs) {
    if (std::none_of(translation.inputs.begin(), translation.inputs.end(),
                     [&](const auto& other) { return SameVariable(input, other); })) {
      why = "input " + VariableName(input) + " is not one of the translation's";
      return false;
    }
  }
  if (replacement.outputs.size() != translation.outputs.size() ||
      !std::equal(replacement.outputs.begin(), replacement.outputs.end(),
                  translation.outputs.begin(), SameVariable)) {
    for (const SpirvInterface::Variable& output : translation.outputs) {
      if (std::none_of(replacement.outputs.begin(), replacement.outputs.end(),
                       [&](const auto& other) { return SameVariable(output, other); })) {
        why = "it does not write output " + VariableName(output);
        return false;
      }
    }
    for (const SpirvInterface::Variable& output : replacement.outputs) {
      if (std::none_of(translation.outputs.begin(), translation.outputs.end(),
                       [&](const auto& other) { return SameVariable(output, other); })) {
        why = "output " + VariableName(output) + " is not one of the translation's";
        return false;
      }
    }
  }
  // ExecutionModelFragment: a fragment shader may leave depth or the sample
  // mask to the rasterizer; earlier stages feed the next one.
  constexpr uint32_t kExecutionModelFragment = 4;
  if (replacement.execution_model != kExecutionModelFragment) {
    for (uint32_t builtin : translation.builtin_outputs) {
      if (!std::binary_search(replacement.builtin_outputs.begin(),
                              replacement.builtin_outputs.end(), builtin)) {
        why = fmt::format("it does not write built-in {}", builtin);
        return false;
      }
    }
  }
  return true;
}

std::vector<uint8_t> LoadVulkanShaderOverride(const std::vector<std::filesystem::path>& roots,
                                              uint64_t guest, uint64_t modification,
                                              std::span<const uint8_t> original,
                                              std::string& source,
                                              const std::function<void(const std::string&)>& warn) {
  source.clear();
  if (roots.empty()) return {};
  std::vector<std::string> filenames = {VulkanShaderOverrideFilename(guest, modification, original)};
  for (std::string& name : VulkanShaderOverrideGuestFilenames(guest, modification))
    filenames.push_back(std::move(name));
  for (size_t name_index = 0; name_index < filenames.size(); ++name_index) {
    // A guest name is optional: its absence is not worth a word.
    const bool exact = name_index == 0;
    for (const auto& root : roots) {
      const auto path = root / filenames[name_index];
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      if (!file) {
        std::error_code error;
        if (std::filesystem::exists(path, error) || (exact && error)) {
          warn(fmt::format("{} rejected: cannot open SPIR-V", rex::path_to_utf8(path)));
        }
        continue;
      }
      const std::streamoff size = file.tellg();
      std::string error;
      std::vector<uint8_t> bytes;
      if (size < 20 || size > std::streamoff(kMaximumBytes) || size % 4) {
        error = "invalid SPIR-V size";
      } else {
        bytes.resize(size_t(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
          error = "cannot read SPIR-V";
        } else if (Validate(bytes, original, error)) {
          source = rex::path_to_utf8(path);
          return bytes;
        }
      }
      warn(fmt::format("{} rejected for {:016X}/{:016X}: {}", rex::path_to_utf8(path), guest,
                       modification, error));
    }
  }
  return {};
}

}  // namespace rex::graphics::vulkan
