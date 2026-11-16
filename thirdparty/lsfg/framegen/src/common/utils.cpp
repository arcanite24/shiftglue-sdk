#include "volk.h"
#include <vulkan/vulkan_core.h>

#include "common/utils.hpp"
#include "core/buffer.hpp"
#include "core/image.hpp"
#include "core/device.hpp"
#include "core/commandpool.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
#include <ios>
#include <system_error>
#include <vector>

using namespace LSFG;
using namespace LSFG::Utils;

BarrierBuilder& BarrierBuilder::addR2W(Core::Image& image) {
    this->barriers.emplace_back(VkImageMemoryBarrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1
        }
    });
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    return *this;
}

BarrierBuilder& BarrierBuilder::addW2R(Core::Image& image) {
    this->barriers.emplace_back(VkImageMemoryBarrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1
        }
    });
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    return *this;
}

void BarrierBuilder::build() const {
    vkCmdPipelineBarrier(this->commandBuffer->handle(),
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        0, nullptr, 0, nullptr,
        static_cast<uint32_t>(this->barriers.size()), this->barriers.data());
}


