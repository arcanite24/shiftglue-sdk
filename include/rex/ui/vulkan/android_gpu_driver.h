/**
 * @file        rex/ui/vulkan/android_gpu_driver.h
 * @brief       Custom Vulkan drivers and GPU clocks on Android (libadrenotools)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <filesystem>

namespace rex::ui::vulkan {

// With android_gpu_driver set, loads that driver (such as Mesa Turnip) from
// drivers_root/<name> through libadrenotools and returns the handle to use as
// libvulkan.so, or nullptr to use the system driver.
void* OpenAndroidCustomVulkanDriver(const std::filesystem::path& drivers_root);

// The highest Adreno GPU clocks while the game is shown and android_gpu_turbo
// is on; the driver's own power control otherwise.
void SetAndroidGpuTurbo(bool shown);

}  // namespace rex::ui::vulkan
