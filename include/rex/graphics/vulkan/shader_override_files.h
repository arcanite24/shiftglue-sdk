#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace rex::graphics::vulkan {

// The exact identity: shader_<guest>_<modification>_<XXH3 of the translated
// SPIR-V>.spv. The translated hash changes with the resolution scale, the
// platform and any translator change.
std::string VulkanShaderOverrideFilename(uint64_t guest, uint64_t modification,
                                         std::span<const uint8_t> original);

// The names a replacement may also use, tried in this order after every
// root's exact name: shader_<guest>_<modification>.spv, then
// shader_<guest>.spv. One such file stands in for every translation of that
// guest shader whose interface it fits (SpirvInterfaceFits).
std::vector<std::string> VulkanShaderOverrideGuestFilenames(uint64_t guest, uint64_t modification);

// What a module's main entry point binds, reads and writes: enough to tell
// whether a replacement can take a translation's place in the translation's
// pipeline layout and between its pipeline's stages.
struct SpirvInterface {
  // SPIR-V ExecutionModel of main (0 vertex, 4 fragment, ...).
  uint32_t execution_model = ~0u;
  struct Resource {
    uint32_t set = 0;
    uint32_t binding = 0;
    // "ubo", "ssbo", "sampler", or the image type ("image f32 2D arr0 ms0
    // sampled1"), "sampled " before it for a combined image and sampler.
    std::string kind;
    // Array elements: 0 when not an array, ~0u for a runtime array.
    uint32_t count = 0;
    // A buffer block's size in bytes (its last member's end).
    uint32_t bytes = 0;
  };
  std::vector<Resource> resources;
  struct Variable {
    uint32_t location = ~0u;
    uint32_t component = 0;
    // "f32x4", "u32", "f32x4[16]", ...
    std::string type;
  };
  std::vector<Variable> inputs;
  std::vector<Variable> outputs;
  // SPIR-V BuiltIn values, from variables and from blocks' members.
  std::vector<uint32_t> builtin_inputs;
  std::vector<uint32_t> builtin_outputs;
  uint32_t push_constant_bytes = 0;
};

bool ReflectSpirvInterface(std::span<const uint32_t> words, SpirvInterface& out,
                           std::string& error);

// Whether a replacement with interface `replacement` can stand in for a
// translation with interface `translation`: the same stage; every resource it
// declares is one of the translation's (same set, binding and kind, no more
// array elements or block bytes), so the translation's layout serves it; no
// more push constants; only inputs the translation has; exactly the
// translation's outputs (and, before the fragment stage, at least its
// built-in outputs). On false, `why` says what differs.
// With host inputs on, a fragment replacement may also declare them.
bool SpirvInterfaceFits(const SpirvInterface& replacement, const SpirvInterface& translation,
                        std::string& why);

// Host inputs for replacements (issue #5 SR-10,
// vulkan_shader_override_host_inputs): with them on, descriptor set 0 of
// every guest pipeline also holds these, which only replacement pixel shaders
// read (translations never do), and the interface check lets a replacement
// declare them.
//   layout(set = 0, binding = 5) uniform sampler2D host_blue_noise;
//     64 x 64 RGBA8 tileable blue noise, four independent channels, sampled
//     with nearest filtering and repeat addressing.
//   layout(set = 0, binding = 6) uniform HostInputs { ... } host;
//     HostInputConstants below (std140).
//   layout(set = 0, binding = 7) uniform sampler2D host_scene_ao;
//     Ambient occlusion of the frame's scene, laid out like the scene
//     texture the composite samples (FH2: turned 180 degrees against the
//     output), 1
//     unoccluded, the strength already applied, in .r; sample by the
//     scene's UV (linear, clamp). 1 everywhere unless
//     swap_ambient_occlusion_in_scene computed it before the hooked draw
//     this frame (scene_ambient_occlusion.cpp).
constexpr uint32_t kHostInputBlueNoiseBinding = 5;
constexpr uint32_t kHostInputConstantsBinding = 6;
constexpr uint32_t kHostInputSceneAoBinding = 7;
constexpr uint32_t kHostInputSceneCopyBinding = 8;
constexpr uint32_t kHostInputBlueNoiseSize = 64;
struct HostInputConstants {
  // The draw resolution scale, the factor translations bake into their
  // texel offsets.
  float draw_resolution_scale[2];
  // 1 / kHostInputBlueNoiseSize.
  float blue_noise_texel;
  // Bumped when the constants change.
  uint32_t version;
  // SR-12: what binding 8 is multiplied by to be in the scene texture's
  // units (2^ the resolve's exponent bias), 1 until a copy is made.
  float scene_copy_scale;
  float reserved[3];
};
static_assert(sizeof(HostInputConstants) == 32);
void SetVulkanShaderOverrideHostInputs(bool enabled);
bool VulkanShaderOverrideHostInputs();
// `size` x `size` RGBA8 blue noise (void and cluster, one run per channel).
std::vector<uint8_t> GenerateBlueNoiseRgba8(uint32_t size);

// The first valid file among every root's exact name, then the guest names
// (VulkanShaderOverrideGuestFilenames) root by root. A file is valid when it
// is SPIR-V that validates for Vulkan 1.2, has a main of the translation's
// stage and fits the translation's interface. Bad files are reported through
// `warn` and never reach the driver.
std::vector<uint8_t> LoadVulkanShaderOverride(const std::vector<std::filesystem::path>& roots,
                                              uint64_t guest, uint64_t modification,
                                              std::span<const uint8_t> original,
                                              std::string& source,
                                              const std::function<void(const std::string&)>& warn);

}  // namespace rex::graphics::vulkan
