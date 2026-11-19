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
#include <pthread.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
// Turnip's ir3 hoists each shader's uniform work into a preamble that runs
// before the draw's waves. FH1 draws are many and small (about 1,800 a
// frame), and without preambles the Odin 2 Portal's frame-600 replay falls
// from 13.89 to 13.65 ms, and the shadows drive has 8 % of frames over 17.5
// ms instead of 12 %.
REXCVAR_DEFINE_BOOL(android_gpu_driver_no_preamble, true, "UI/Vulkan",
                    "Have a custom Mesa driver compile shaders without preambles "
                    "(IR3_SHADER_DEBUG=nopreamble), unless android_gpu_driver_env or the "
                    "environment sets IR3_SHADER_DEBUG")
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

// The dynamic string table and the DT_SONAME and DT_NEEDED string offsets of
// an ELF64 shared library, or false when it is not one.
struct ElfDynamicNames {
  size_t strtab = 0;
  std::vector<std::pair<int64_t, uint64_t>> entries;  // tag, string offset
};

bool ReadElfDynamicNames(const std::vector<uint8_t>& data, ElfDynamicNames& names) {
  auto read = [&](size_t at, auto& value) {
    if (at + sizeof(value) > data.size()) return false;
    std::memcpy(&value, data.data() + at, sizeof(value));
    return true;
  };
  if (data.size() < 64 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0 || data[4] != 2) {
    return false;
  }
  uint64_t phoff = 0;
  uint16_t phentsize = 0, phnum = 0;
  if (!read(0x20, phoff) || !read(0x36, phentsize) || !read(0x38, phnum)) return false;
  struct Load {
    uint64_t vaddr, offset, size;
  };
  std::vector<Load> loads;
  uint64_t dynamic_offset = 0, dynamic_size = 0;
  for (uint16_t i = 0; i < phnum; i++) {
    const size_t at = size_t(phoff) + size_t(i) * phentsize;
    uint32_t type = 0;
    uint64_t offset = 0, vaddr = 0, filesz = 0;
    if (!read(at, type) || !read(at + 8, offset) || !read(at + 16, vaddr) ||
        !read(at + 32, filesz)) {
      return false;
    }
    if (type == 1) loads.push_back({vaddr, offset, filesz});
    if (type == 2) {
      dynamic_offset = offset;
      dynamic_size = filesz;
    }
  }
  uint64_t strtab_vaddr = 0;
  bool have_strtab = false;
  for (uint64_t at = dynamic_offset; at + 16 <= dynamic_offset + dynamic_size; at += 16) {
    int64_t tag = 0;
    uint64_t value = 0;
    if (!read(size_t(at), tag) || !read(size_t(at + 8), value) || tag == 0) break;
    if (tag == 5) {  // DT_STRTAB
      strtab_vaddr = value;
      have_strtab = true;
    } else if (tag == 1 || tag == 14) {  // DT_NEEDED, DT_SONAME
      names.entries.emplace_back(tag, value);
    }
  }
  for (const Load& load : loads) {
    if (have_strtab && strtab_vaddr >= load.vaddr && strtab_vaddr < load.vaddr + load.size) {
      names.strtab = size_t(strtab_vaddr - load.vaddr + load.offset);
      return true;
    }
  }
  return false;
}

std::string ElfString(const std::vector<uint8_t>& data, size_t at) {
  if (at >= data.size()) return {};
  const auto* begin = reinterpret_cast<const char*>(data.data() + at);
  return std::string(begin, strnlen(begin, data.size() - at));
}

// Qualcomm driver packages ship vulkan.adreno.so with libgsl.so,
// libllvm-glnext.so and other libraries under the system driver's sonames.
// The system driver is usually loaded already (the UI renders with Vulkan),
// and the linker returns a loaded library for a matching soname, so the
// package would silently run the system driver. Gives every library in the
// package a unique soname of the same length (the first letter replaced, from
// 'p' on, by one no other name in the package uses), patches the DT_SONAME and
// DT_NEEDED strings that name them and renames the files. Returns the driver
// library's new name, or empty.
std::string MakeDriverSonamesUnique(const std::filesystem::path& folder,
                                    const std::string& library) {
  struct Library {
    std::filesystem::path path;
    std::vector<uint8_t> data;
    ElfDynamicNames names;
    std::string soname;
  };
  std::vector<Library> libraries;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (entry.path().extension() != ".so") continue;
    Library lib;
    lib.path = entry.path();
    std::ifstream file(lib.path, std::ios::binary);
    lib.data.assign(std::istreambuf_iterator<char>(file), {});
    if (!ReadElfDynamicNames(lib.data, lib.names)) continue;
    lib.soname = lib.path.filename().string();
    for (const auto& [tag, offset] : lib.names.entries) {
      if (tag == 14) lib.soname = ElfString(lib.data, lib.names.strtab + offset);
    }
    if (!lib.soname.empty()) libraries.push_back(std::move(lib));
  }
  std::unordered_set<std::string> taken;
  for (const Library& lib : libraries) {
    taken.insert(lib.soname);
    taken.insert(lib.path.filename().string());
  }
  std::unordered_map<std::string, std::string> renamed;
  std::string renamed_library;
  for (const Library& lib : libraries) {
    std::string name = lib.soname;
    for (char letter = 'p'; letter <= 'z' && taken.count(name); letter++) name[0] = letter;
    if (taken.count(name)) return {};
    taken.insert(name);
    renamed[lib.soname] = name;
    if (lib.path.filename() == library) renamed_library = name;
  }
  for (Library& lib : libraries) {
    for (const auto& [tag, offset] : lib.names.entries) {
      const size_t at = lib.names.strtab + offset;
      const std::string name = ElfString(lib.data, at);
      if (const auto found = renamed.find(name); found != renamed.end()) {
        const std::string& replacement = found->second;
        std::memcpy(lib.data.data() + at, replacement.data(), replacement.size());
      }
    }
    std::filesystem::remove(lib.path, error);
    std::ofstream out(folder / renamed[lib.soname], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(lib.data.data()), std::streamsize(lib.data.size()));
    if (!out) return {};
  }
  return renamed_library;
}

std::string g_loaded_driver;

// SDL attaches a thread to the JVM on its first JNI call with no name, and
// ART then renames it "Thread-N". The presenter's first paint runs on the GPU
// command decoder, so "GPU Commands" became "Thread-9" (POCO F7 Pro, 0.9.1)
// and android_thread_cores, which matches names, no longer found it. The
// thread keeps its own name.
JNIEnv* AndroidJniEnvKeepingThreadName() {
  char name[16] = {};
  const bool named = pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 && name[0];
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (named)
    pthread_setname_np(pthread_self(), name);
  return env;
}

}  // namespace

const std::string& LoadedAndroidGpuDriver() { return g_loaded_driver; }

bool CallAndroidActivityMethod(const char* method) {
  auto* env = AndroidJniEnvKeepingThreadName();
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

const AndroidDisplayHdr& GetAndroidDisplayHdr() {
  static const AndroidDisplayHdr hdr = [] {
    AndroidDisplayHdr result;
    if (const char* diagnostics = std::getenv("REX_ANDROID_DISPLAY_DIAGNOSTICS")) {
      REXLOG_INFO("Android display environment (launch snapshot): {}", diagnostics);
    }
    auto* env = AndroidJniEnvKeepingThreadName();
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return result;
    // activity.getDisplay().getHdrCapabilities() (API 30).
    jclass activity_class = env->GetObjectClass(activity);
    jmethodID get_display = env->GetMethodID(activity_class, "getDisplay", "()Landroid/view/Display;");
    jobject display = get_display ? env->CallObjectMethod(activity, get_display) : nullptr;
    jobject caps = nullptr;
    if (display && !env->ExceptionCheck()) {
      jclass display_class = env->GetObjectClass(display);
      jmethodID get_caps = env->GetMethodID(display_class, "getHdrCapabilities",
                                            "()Landroid/view/Display$HdrCapabilities;");
      caps = get_caps ? env->CallObjectMethod(display, get_caps) : nullptr;
      env->DeleteLocalRef(display_class);
    }
    if (caps && !env->ExceptionCheck()) {
      jclass caps_class = env->GetObjectClass(caps);
      jmethodID get_types = env->GetMethodID(caps_class, "getSupportedHdrTypes", "()[I");
      auto types = get_types ? static_cast<jintArray>(env->CallObjectMethod(caps, get_types))
                             : nullptr;
      if (types && !env->ExceptionCheck()) {
        const jsize count = env->GetArrayLength(types);
        jint* values = env->GetIntArrayElements(types, nullptr);
        for (jsize i = 0; values && i < count; ++i) {
          // Display.HdrCapabilities.HDR_TYPE_HDR10.
          result.hdr10 |= values[i] == 2;
        }
        if (values) env->ReleaseIntArrayElements(types, values, JNI_ABORT);
        env->DeleteLocalRef(types);
      }
      const auto get_float = [&](const char* name) {
        jmethodID id = env->GetMethodID(caps_class, name, "()F");
        float value = id ? env->CallFloatMethod(caps, id) : 0.0f;
        if (env->ExceptionCheck()) {
          env->ExceptionClear();
          value = 0.0f;
        }
        return value;
      };
      result.max_nits = get_float("getDesiredMaxLuminance");
      result.max_average_nits = get_float("getDesiredMaxAverageLuminance");
      result.min_nits = get_float("getDesiredMinLuminance");
      env->DeleteLocalRef(caps_class);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (caps) env->DeleteLocalRef(caps);
    env->DeleteLocalRef(activity_class);
    env->DeleteLocalRef(activity);
    REXLOG_INFO("Android display HDR: HDR10 {}, peak {} nits, average {} nits, black {} nits",
                result.hdr10 ? "yes" : "no", result.max_nits, result.max_average_nits,
                result.min_nits);
    return result;
  }();
  return hdr;
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
  std::string library = DriverLibraryName(target);
  if (library.empty()) {
    REXLOG_ERROR("Custom GPU driver {}: no driver library in {}", name, source.string());
    return nullptr;
  }
  library = MakeDriverSonamesUnique(target, library);
  if (library.empty()) {
    REXLOG_ERROR("Custom GPU driver {}: cannot rename the libraries in {}", name,
                 target.string());
    return nullptr;
  }
  // Settings the driver reads from the environment when it loads.
  if (REXCVAR_GET(android_gpu_driver_no_preamble) && !getenv("IR3_SHADER_DEBUG")) {
    setenv("IR3_SHADER_DEBUG", "nopreamble", 0);
    REXLOG_INFO("Custom GPU driver {}: IR3_SHADER_DEBUG=nopreamble", name);
  }
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
