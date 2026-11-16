#pragma once

// ShiftGlue: stands in for volk. lsfg-vk loaded every Vulkan entry point
// globally through volk; here they come from the presenter's device, in one
// table, and the vk* names below resolve to it.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#define LSFG_VK_INSTANCE_FUNCTIONS(X) X(vkGetPhysicalDeviceMemoryProperties)

#define LSFG_VK_DEVICE_FUNCTIONS(X)                                              \
  X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory)    \
  X(vkBeginCommandBuffer) X(vkBindBufferMemory) X(vkBindImageMemory)             \
  X(vkCmdBindDescriptorSets) X(vkCmdBindPipeline) X(vkCmdCopyImage)              \
  X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkCreateBuffer)                     \
  X(vkCreateCommandPool) X(vkCreateComputePipelines) X(vkCreateDescriptorPool)   \
  X(vkCreateDescriptorSetLayout) X(vkCreateImage) X(vkCreateImageView)           \
  X(vkCreatePipelineLayout) X(vkCreateSampler) X(vkCreateShaderModule)           \
  X(vkDestroyBuffer) X(vkDestroyCommandPool) X(vkDestroyDescriptorPool)          \
  X(vkDestroyDescriptorSetLayout) X(vkDestroyImage) X(vkDestroyImageView)        \
  X(vkDestroyPipeline) X(vkDestroyPipelineLayout) X(vkDestroySampler)            \
  X(vkDestroyShaderModule) X(vkEndCommandBuffer) X(vkFreeCommandBuffers)         \
  X(vkFreeDescriptorSets) X(vkFreeMemory) X(vkGetBufferMemoryRequirements)       \
  X(vkGetImageMemoryRequirements) X(vkMapMemory) X(vkUnmapMemory)                \
  X(vkUpdateDescriptorSets)

namespace LSFG {

struct VkTable {
#define LSFG_VK_MEMBER(name) PFN_##name name = nullptr;
  LSFG_VK_INSTANCE_FUNCTIONS(LSFG_VK_MEMBER)
  LSFG_VK_DEVICE_FUNCTIONS(LSFG_VK_MEMBER)
#undef LSFG_VK_MEMBER
};

extern VkTable g_vk;

// Fills g_vk for this device; false if any entry point is missing.
bool LoadVkTable(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
                 PFN_vkGetDeviceProcAddr get_device_proc_addr, VkDevice device);

}  // namespace LSFG

#ifndef LSFG_VK_NO_ALIASES
#define LSFG_VK_ALIAS(name) ::LSFG::g_vk.name
#define vkGetPhysicalDeviceMemoryProperties LSFG_VK_ALIAS(vkGetPhysicalDeviceMemoryProperties)
#define vkAllocateCommandBuffers LSFG_VK_ALIAS(vkAllocateCommandBuffers)
#define vkAllocateDescriptorSets LSFG_VK_ALIAS(vkAllocateDescriptorSets)
#define vkAllocateMemory LSFG_VK_ALIAS(vkAllocateMemory)
#define vkBeginCommandBuffer LSFG_VK_ALIAS(vkBeginCommandBuffer)
#define vkBindBufferMemory LSFG_VK_ALIAS(vkBindBufferMemory)
#define vkBindImageMemory LSFG_VK_ALIAS(vkBindImageMemory)
#define vkCmdBindDescriptorSets LSFG_VK_ALIAS(vkCmdBindDescriptorSets)
#define vkCmdBindPipeline LSFG_VK_ALIAS(vkCmdBindPipeline)
#define vkCmdCopyImage LSFG_VK_ALIAS(vkCmdCopyImage)
#define vkCmdDispatch LSFG_VK_ALIAS(vkCmdDispatch)
#define vkCmdPipelineBarrier LSFG_VK_ALIAS(vkCmdPipelineBarrier)
#define vkCreateBuffer LSFG_VK_ALIAS(vkCreateBuffer)
#define vkCreateCommandPool LSFG_VK_ALIAS(vkCreateCommandPool)
#define vkCreateComputePipelines LSFG_VK_ALIAS(vkCreateComputePipelines)
#define vkCreateDescriptorPool LSFG_VK_ALIAS(vkCreateDescriptorPool)
#define vkCreateDescriptorSetLayout LSFG_VK_ALIAS(vkCreateDescriptorSetLayout)
#define vkCreateImage LSFG_VK_ALIAS(vkCreateImage)
#define vkCreateImageView LSFG_VK_ALIAS(vkCreateImageView)
#define vkCreatePipelineLayout LSFG_VK_ALIAS(vkCreatePipelineLayout)
#define vkCreateSampler LSFG_VK_ALIAS(vkCreateSampler)
#define vkCreateShaderModule LSFG_VK_ALIAS(vkCreateShaderModule)
#define vkDestroyBuffer LSFG_VK_ALIAS(vkDestroyBuffer)
#define vkDestroyCommandPool LSFG_VK_ALIAS(vkDestroyCommandPool)
#define vkDestroyDescriptorPool LSFG_VK_ALIAS(vkDestroyDescriptorPool)
#define vkDestroyDescriptorSetLayout LSFG_VK_ALIAS(vkDestroyDescriptorSetLayout)
#define vkDestroyImage LSFG_VK_ALIAS(vkDestroyImage)
#define vkDestroyImageView LSFG_VK_ALIAS(vkDestroyImageView)
#define vkDestroyPipeline LSFG_VK_ALIAS(vkDestroyPipeline)
#define vkDestroyPipelineLayout LSFG_VK_ALIAS(vkDestroyPipelineLayout)
#define vkDestroySampler LSFG_VK_ALIAS(vkDestroySampler)
#define vkDestroyShaderModule LSFG_VK_ALIAS(vkDestroyShaderModule)
#define vkEndCommandBuffer LSFG_VK_ALIAS(vkEndCommandBuffer)
#define vkFreeCommandBuffers LSFG_VK_ALIAS(vkFreeCommandBuffers)
#define vkFreeDescriptorSets LSFG_VK_ALIAS(vkFreeDescriptorSets)
#define vkFreeMemory LSFG_VK_ALIAS(vkFreeMemory)
#define vkGetBufferMemoryRequirements LSFG_VK_ALIAS(vkGetBufferMemoryRequirements)
#define vkGetImageMemoryRequirements LSFG_VK_ALIAS(vkGetImageMemoryRequirements)
#define vkMapMemory LSFG_VK_ALIAS(vkMapMemory)
#define vkUnmapMemory LSFG_VK_ALIAS(vkUnmapMemory)
#define vkUpdateDescriptorSets LSFG_VK_ALIAS(vkUpdateDescriptorSets)
#endif  // LSFG_VK_NO_ALIASES
