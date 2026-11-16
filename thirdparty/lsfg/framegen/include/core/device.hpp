#pragma once

// ShiftGlue: a non-owning view of the presenter's Vulkan device (lsfg-vk
// created its own device from a UUID).

#include <vulkan/vulkan_core.h>

#include <cstdint>

namespace LSFG::Core {

    class Device {
    public:
        Device() noexcept = default;
        Device(VkDevice device, VkPhysicalDevice physicalDevice,
            uint32_t computeFamilyIdx, VkQueue computeQueue)
            : device(device), physicalDevice(physicalDevice),
              computeFamilyIdx(computeFamilyIdx), computeQueue(computeQueue) {}

        [[nodiscard]] VkDevice handle() const { return this->device; }
        [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const { return this->physicalDevice; }
        [[nodiscard]] uint32_t getComputeFamilyIdx() const { return this->computeFamilyIdx; }
        [[nodiscard]] VkQueue getComputeQueue() const { return this->computeQueue; }

    private:
        VkDevice device{};
        VkPhysicalDevice physicalDevice{};
        uint32_t computeFamilyIdx{0};
        VkQueue computeQueue{};
    };

}
