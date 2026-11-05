/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                   *
 ******************************************************************************
 * MetalFX frame interpolation as a frame generation backend for the Vulkan
 * presenter on MoltenVK: the presenter's images are exported to Metal through
 * VK_EXT_metal_objects and the interpolator runs on a Metal command buffer,
 * ordered against the Vulkan work by a timeline semaphore exported as a
 * Metal shared event.
 */

#pragma once

#include <memory>

#include <rex/ui/frame_generator.h>
#include <rex/ui/vulkan/frame_generator.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

// Built only on macOS with REXGLUE_ENABLE_METALFX; elsewhere the functions
// report the backend unavailable.
FrameGenerationAvailability ProbeMetalFxFrameGenerator(const VulkanDevice* vulkan_device);
std::unique_ptr<VulkanFrameGenerator> CreateMetalFxFrameGenerator(
    const VulkanDevice* vulkan_device);

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
