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
#include <string>

namespace rex::ui::vulkan {

// With android_gpu_driver set, loads that driver (such as Mesa Turnip) from
// drivers_root/<name> through libadrenotools and returns the handle to use as
// libvulkan.so, or nullptr to use the system driver. "auto" takes the driver
// the app recommends for the device's GPU (REX_ANDROID_RECOMMENDED_DRIVER).
void* OpenAndroidCustomVulkanDriver(const std::filesystem::path& drivers_root);

// The custom driver loaded at startup (its folder name), or empty for the
// system driver.
const std::string& LoadedAndroidGpuDriver();

// Calls a no-argument void method of the app's Android activity (Java), such
// as one that opens a document picker; false when it has no such method.
bool CallAndroidActivityMethod(const char* method);

// The highest Adreno GPU clocks while the game is shown and android_gpu_turbo
// is on; the driver's own power control otherwise.
void SetAndroidGpuTurbo(bool shown);

}  // namespace rex::ui::vulkan
