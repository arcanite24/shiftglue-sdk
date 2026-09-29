#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/fh1_frame_dump.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;
class VulkanSharedMemory;

// Frame dump chunks as host-visible buffers filled by copies from the shared
// memory buffer.
class VulkanFh1FrameDumpMirror : public Fh1FrameDumpMirror {
 public:
  VulkanFh1FrameDumpMirror(VulkanCommandProcessor& command_processor,
                           VulkanSharedMemory& shared_memory)
      : command_processor_(command_processor), shared_memory_(shared_memory) {}
  ~VulkanFh1FrameDumpMirror() override { Release(); }

  bool CreateChunk(uint32_t size) override;
  bool Copy(uint32_t chunk, uint32_t offset, uint32_t address, uint32_t length) override;
  std::vector<const uint8_t*> AwaitAndMap() override;
  void Release() override;

 private:
  struct Chunk {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool mapped = false;
  };
  VulkanCommandProcessor& command_processor_;
  VulkanSharedMemory& shared_memory_;
  std::vector<Chunk> chunks_;
};

}  // namespace rex::graphics::vulkan
