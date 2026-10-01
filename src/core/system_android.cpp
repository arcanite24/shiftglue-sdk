/**
 * @file        core/system_android.cpp
 * @brief       Android process-wide platform state
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/main_android.h>

#include <android/api-level.h>

#include <rex/filesystem.h>
#include <rex/memory/utils.h>
#include <rex/system.h>
#include <rex/thread.h>

namespace rex {

namespace {
int android_api_level_ = 0;
}  // namespace

int GetAndroidApiLevel() {
  if (!android_api_level_) {
    android_api_level_ = android_get_device_api_level();
  }
  return android_api_level_;
}

bool InitializeAndroidSystemForApplicationContext() {
  GetAndroidApiLevel();
  rex::thread::AndroidInitialize();
  rex::memory::AndroidInitialize();
  rex::filesystem::AndroidInitialize();
  return true;
}

void ShutdownAndroidSystem() {
  rex::filesystem::AndroidShutdown();
  rex::memory::AndroidShutdown();
  rex::thread::AndroidShutdown();
}

namespace filesystem {

// Game data and state live in app-specific storage reached by plain paths,
// so content URIs (a storage-access-framework folder picker) are not used yet.
void AndroidInitialize() {}

void AndroidShutdown() {}

bool IsAndroidContentUri(const std::string_view source) {
  return source.starts_with("content://");
}

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  (void)uri;
  (void)mode;
  return -1;
}

}  // namespace filesystem

}  // namespace rex