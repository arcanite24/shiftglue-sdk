#include "volk.h"
#include <vulkan/vulkan_core.h>

#include "core/commandbuffer.hpp"
#include "core/device.hpp"
#include "core/commandpool.hpp"
#include "common/exception.hpp"

#include <memory>
#include <stdexcept>
#include <cstdint>
#include <optional>
#include <vector>

using namespace LSFG::Core;

CommandBuffer::CommandBuffer(const Core::Device& device, const CommandPool& pool) {
    // create command buffer
    const VkCommandBufferAllocateInfo desc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool.handle(),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer commandBufferHandle{};
    auto res = vkAllocateCommandBuffers(device.handle(), &desc, &commandBufferHandle);
    if (res != VK_SUCCESS || commandBufferHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to allocate command buffer");

    // store command buffer in shared ptr
    this->state = std::make_shared<CommandBufferState>(CommandBufferState::Empty);
    this->commandBuffer = std::shared_ptr<VkCommandBuffer>(
        new VkCommandBuffer(commandBufferHandle),
        [dev = device.handle(), pool = pool.handle()](VkCommandBuffer* cmdBuffer) {
            vkFreeCommandBuffers(dev, pool, 1, cmdBuffer);
        }
    );
}

CommandBuffer::CommandBuffer(VkCommandBuffer external) {
    this->state = std::make_shared<CommandBufferState>(CommandBufferState::Recording);
    this->commandBuffer = std::make_shared<VkCommandBuffer>(external);
}

void CommandBuffer::begin() {
    if (*this->state != CommandBufferState::Empty)
        throw std::logic_error("Command buffer is not in Empty state");

    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    auto res = vkBeginCommandBuffer(*this->commandBuffer, &beginInfo);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to begin command buffer");

    *this->state = CommandBufferState::Recording;
}

void CommandBuffer::dispatch(uint32_t x, uint32_t y, uint32_t z) const {
    if (*this->state != CommandBufferState::Recording)
        throw std::logic_error("Command buffer is not in Recording state");

    vkCmdDispatch(*this->commandBuffer, x, y, z);
}

void CommandBuffer::end() {
    if (*this->state != CommandBufferState::Recording)
        throw std::logic_error("Command buffer is not in Recording state");

    auto res = vkEndCommandBuffer(*this->commandBuffer);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to end command buffer");

    *this->state = CommandBufferState::Full;
}

