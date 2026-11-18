#pragma once

// ShiftGlue: LSFG 3.1 run inside a host's Vulkan presenter. lsfg-vk 1.0.0
// ran the passes on a device of its own and shared images and semaphores
// with the game through file descriptors; here they run on the host's
// device and are recorded into the host's command buffer, with the real
// frame copied in and the generated one copied out.

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lsfg {

struct EmbeddedDevice {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
};

struct EmbeddedSettings {
  // LSFG 3.1's performance mode (the p_* shaders).
  bool performance = false;
  // The motion search resolution as a share of the frame, 0.25 to 1.
  float flow_scale = 1.0f;
  // Frames generated between two real ones (1 to 3: 2x to 4x).
  uint32_t generation_count = 1;
};

// SPIR-V for a shader name ("mipmaps", "p_alpha[0]", ...), empty if missing.
using ShaderSource = std::function<std::vector<uint8_t>(const std::string&)>;

class Embedded {
 public:
  // One at a time per process: the passes share one function table.
  static std::unique_ptr<Embedded> Create(const EmbeddedDevice& device, ShaderSource shaders,
                                          const EmbeddedSettings& settings, std::string* error);
  virtual ~Embedded() = default;

  // Images for frames of this size (RGBA8); kept if they already fit. The
  // previous ones must be out of use by the GPU.
  virtual bool Resize(uint32_t width, uint32_t height, std::string* error) = 0;

  // Records: present_color (SHADER_READ_ONLY_OPTIMAL, left that way, with
  // TRANSFER_SRC usage) into the history, the passes, and the generated
  // frames between the previous one and it, in order, into outputs (one per
  // generation_count; GENERAL, left GENERAL and visible to compute
  // shaders). Returns false when nothing was written: the first frames after
  // Resize, or when generate is false (a cut), which still records the
  // history so the next frame can generate.
  virtual bool Record(VkCommandBuffer command_buffer, VkImage present_color,
                      const VkImage* outputs, bool generate) = 0;
};

}  // namespace lsfg
