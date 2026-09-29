#include <rex/graphics/vulkan/fh1_frame_dump.h>

#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {

bool VulkanFh1FrameDumpMirror::CreateChunk(uint32_t size) {
  Chunk chunk;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(), size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, chunk.buffer, chunk.memory)) {
    return false;
  }
  chunks_.push_back(chunk);
  return true;
}

bool VulkanFh1FrameDumpMirror::Copy(uint32_t chunk, uint32_t offset, uint32_t address,
                                    uint32_t length) {
  if (chunk >= chunks_.size() || !shared_memory_.RequestRange(address, length)) return false;
  shared_memory_.Use(VulkanSharedMemory::Usage::kRead);
  command_processor_.SubmitBarriers(true);
  const VkBufferCopy region = {address, offset, length};
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(
      shared_memory_.buffer(), chunks_[chunk].buffer, 1, &region);
  return true;
}

std::vector<const uint8_t*> VulkanFh1FrameDumpMirror::AwaitAndMap() {
  command_processor_.Fh1AwaitAllQueueOperations();
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  std::vector<const uint8_t*> mapped(chunks_.size(), nullptr);
  for (size_t i = 0; i < chunks_.size(); ++i) {
    void* pointer = nullptr;
    if (dfn.vkMapMemory(device, chunks_[i].memory, 0, VK_WHOLE_SIZE, 0, &pointer) != VK_SUCCESS) {
      continue;
    }
    VkMappedMemoryRange range = {};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = chunks_[i].memory;
    range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
    chunks_[i].mapped = true;
    mapped[i] = static_cast<const uint8_t*>(pointer);
  }
  return mapped;
}

void VulkanFh1FrameDumpMirror::Release() {
  if (chunks_.empty()) return;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (Chunk& chunk : chunks_) {
    if (chunk.mapped) dfn.vkUnmapMemory(device, chunk.memory);
    dfn.vkDestroyBuffer(device, chunk.buffer, nullptr);
    dfn.vkFreeMemory(device, chunk.memory, nullptr);
  }
  chunks_.clear();
}

}  // namespace rex::graphics::vulkan
