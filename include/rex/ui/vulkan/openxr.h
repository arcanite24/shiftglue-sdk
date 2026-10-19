#pragma once
/**
 * @file        rex/ui/vulkan/openxr.h
 * @brief       The title's output in a VR headset through OpenXR.
 *
 * With xr_enabled, the Vulkan instance and device are created with the
 * extensions the player's OpenXR runtime asks for (XR_KHR_vulkan_enable), a
 * session opens on that device, and every presented frame is copied into the
 * headset as a large screen in front of the player. Builds without
 * REX_HAS_OPENXR (cmake/rexglue_openxr.cmake) compile these to no-ops.
 *
 * @license     BSD 3-Clause License
 */

#include <cstdint>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;
class VulkanInstance;

namespace openxr {

// Extensions the runtime needs on the instance, and on this physical device.
// Empty when xr_enabled is off, the build has no OpenXR, or no runtime or
// headset answers. The names stay valid for the life of the process.
std::vector<const char*> InstanceExtensions();
std::vector<const char*> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical_device);

// Opens the session on the presenter's device, once.
void OnDeviceCreated(const VulkanInstance* instance, const VulkanDevice* device);

// The presenter's per-frame hooks, all on its painting thread. BeginFrame
// waits for the headset's next frame and returns true when the frame wants
// an image of that size; RecordCopy then copies the guest output (in the
// given layout, which it restores) into it in the paint's command buffer.
// EndFrame goes after that command buffer is submitted, whether or not
// BeginFrame returned true.
bool BeginFrame(uint32_t width, uint32_t height);
void RecordCopy(VkCommandBuffer command_buffer, VkImage source, uint32_t width, uint32_t height,
                VkImageLayout source_layout, VkAccessFlags source_access,
                VkPipelineStageFlags source_stage);
void EndFrame();

}  // namespace openxr
}  // namespace vulkan
}  // namespace ui
}  // namespace rex
