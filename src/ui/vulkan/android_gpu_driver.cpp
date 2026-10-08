/**
 * @file        ui/vulkan/android_gpu_driver.cpp
 * @brief       Custom Vulkan drivers and GPU clocks on Android (libadrenotools)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/vulkan/android_gpu_driver.h>

#include <dlfcn.h>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

#include <jni.h>

#include <SDL3/SDL_system.h>
#include <adrenotools/driver.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(android_gpu_driver, "auto", "UI/Vulkan",
                      "A custom Vulkan driver (such as Mesa Turnip) to load instead of the "
                      "system's: the name of a folder under REX_ANDROID_DRIVERS_DIR, holding "
                      "an adrenotools driver package (meta.json and the .so) or a lone .so. "
                      "Empty uses the system driver; auto the driver the app recommends for "
                      "the device's GPU (REX_ANDROID_RECOMMENDED_DRIVER), else the system's")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(android_gpu_driver_env, "", "UI/Vulkan",
                      "Environment for the custom driver, as NAME=value pairs separated by "
                      "semicolons (Mesa Turnip reads TU_DEBUG, for one: TU_DEBUG=sysmem)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(android_gpu_turbo, false, "UI/Vulkan",
                    "Run the Adreno GPU at its highest clocks while the game is shown (thermal "
                    "limits still apply); restored in the background and at exit")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui::vulkan {

namespace {

// The folder the packaged native libraries were extracted to, where the
// adrenotools hooks are: the folder holding this library.
std::filesystem::path NativeLibraryFolder() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&NativeLibraryFolder), &info) || !info.dli_fname) {
    return {};
  }
  return std::filesystem::path(info.dli_fname).parent_path();
}

// The driver's .so: meta.json's libraryName, or the folder's only .so.
std::string DriverLibraryName(const std::filesystem::path& folder) {
  std::ifstream meta(folder / "meta.json");
  if (meta) {
    std::stringstream text;
    text << meta.rdbuf();
    std::smatch match;
    const std::string json = text.str();
    if (std::regex_search(json, match, std::regex(R"("libraryName"\s*:\s*"([^"]+)\")"))) {
      return match[1].str();
    }
  }
  std::string found;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (entry.path().extension() == ".so") {
      if (!found.empty()) {
        return {};  // more than one: meta.json must say
      }
      found = entry.path().filename().string();
    }
  }
  return found;
}

std::string g_loaded_driver;

}  // namespace

const std::string& LoadedAndroidGpuDriver() { return g_loaded_driver; }

bool CallAndroidActivityMethod(const char* method) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return false;
  jclass activity_class = env->GetObjectClass(activity);
  jmethodID id = env->GetMethodID(activity_class, method, "()V");
  bool called = false;
  if (id) {
    env->CallVoidMethod(activity, id);
    called = !env->ExceptionCheck();
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(activity_class);
  env->DeleteLocalRef(activity);
  return called;
}

void* OpenAndroidCustomVulkanDriver(const std::filesystem::path& drivers_root) {
  std::string name = REXCVAR_GET(android_gpu_driver);
  if (name == "auto") {
    const char* recommended = std::getenv("REX_ANDROID_RECOMMENDED_DRIVER");
    std::error_code exists_error;
    if (!recommended || !*recommended ||
        !std::filesystem::is_directory(drivers_root / recommended, exists_error)) {
      REXLOG_INFO("GPU driver auto: no driver recommended for this GPU, using the system's");
      return nullptr;
    }
    name = recommended;
    REXLOG_INFO("GPU driver auto: {} recommended for this GPU", name);
  }
  if (name.empty()) {
    return nullptr;
  }
  const std::filesystem::path source = drivers_root / name;
  const char* internal = SDL_GetAndroidInternalStoragePath();
  const std::filesystem::path hooks = NativeLibraryFolder();
  if (!internal || hooks.empty()) {
    REXLOG_ERROR("Custom GPU driver {}: no internal storage or library folder", name);
    return nullptr;
  }
  // dlopen refuses libraries on shared storage: copy the package into the
  // app's internal storage, where only the app can write.
  const std::filesystem::path target = std::filesystem::path(internal) / "gpu_drivers" / name;
  std::error_code error;
  std::filesystem::remove_all(target, error);
  std::filesystem::create_directories(target, error);
  std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
  if (error) {
    REXLOG_ERROR("Custom GPU driver {}: cannot copy {} ({})", name, source.string(),
                 error.message());
    return nullptr;
  }
  const std::string library = DriverLibraryName(target);
  if (library.empty()) {
    REXLOG_ERROR("Custom GPU driver {}: no driver library in {}", name, source.string());
    return nullptr;
  }
  // Settings the driver reads from the environment when it loads.
  std::stringstream pairs(REXCVAR_GET(android_gpu_driver_env));
  for (std::string pair; std::getline(pairs, pair, ';');) {
    const size_t equals = pair.find('=');
    if (equals != std::string::npos && equals > 0) {
      setenv(pair.substr(0, equals).c_str(), pair.substr(equals + 1).c_str(), 1);
      REXLOG_INFO("Custom GPU driver {}: {}", name, pair);
    }
  }
  const std::string target_dir = target.string() + "/";
  const std::string hook_dir = hooks.string() + "/";
  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                                            hook_dir.c_str(), target_dir.c_str(),
                                            library.c_str(), nullptr, nullptr);
  if (!handle) {
    REXLOG_ERROR("Custom GPU driver {}: adrenotools could not load {} ({})", name, library,
                 dlerror() ? dlerror() : "no reason given");
    return nullptr;
  }
  REXLOG_INFO("Custom GPU driver {}: loaded {} through adrenotools", name, library);
  g_loaded_driver = name;
  return handle;
}

void SetAndroidGpuTurbo(bool shown) {
  const bool turbo = shown && REXCVAR_GET(android_gpu_turbo);
  static bool applied = false;
  if (turbo == applied) {
    return;
  }
  adrenotools_set_turbo(turbo);
  applied = turbo;
  REXLOG_INFO("Adreno GPU turbo {}", turbo ? "on" : "off");
}

}  // namespace rex::ui::vulkan
