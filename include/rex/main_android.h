#pragma once
/**
 * @file        main_android.h
 * @brief       Android process-wide platform state
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

namespace rex {

// The device's API level (android_get_device_api_level), cached at start.
int GetAndroidApiLevel();

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID