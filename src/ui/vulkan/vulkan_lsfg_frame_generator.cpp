/**
 * Lossless Scaling frame generation (UP-5.2).
 *
 * LSFG runs from the player's own Lossless.dll (docs/LEGAL.md in Pinyon
 * Shift: it is never fetched, shipped or cached). This finds the DLL (a path
 * the player sets, or the Lossless Scaling folder in their Steam libraries),
 * reads its DXBC shaders in memory, translates them to SPIR-V with DXVK's
 * compiler and runs lsfg-vk's LSFG 3.1 passes on the presenter's device, in
 * its command buffer (thirdparty/lsfg). Nothing is looked for unless
 * present_lsfg_dll or present_lsfg_experimental is set.
 */

#include <rex/ui/vulkan/frame_generator.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#if REX_HAS_LSFG
#include <exception>
#include <map>

#include "lsfg_embedded.h"
#include "lsfg_shaders.h"
#endif

REXCVAR_DEFINE_STRING(present_lsfg_dll, "", "UI/Presenter",
                      "Path to the player's own Lossless.dll for LSFG frame generation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(present_lsfg_experimental, false, "UI/Presenter",
                    "Look for Lossless.dll in the Steam library folders when "
                    "present_lsfg_dll is empty")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(present_lsfg_performance, false, "UI/Presenter",
                    "LSFG performance mode: lighter passes, softer generated frames")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(present_lsfg_flow_scale, 1.0, "UI/Presenter",
                      "LSFG motion search resolution as a share of the frame (0.25 to 1)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex {
namespace ui {
namespace vulkan {

namespace {

namespace fs = std::filesystem;

std::string GetEnvironment(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

// The Steam library folders: each install's own folder, then the paths its
// libraryfolders.vdf lists.
std::vector<fs::path> GetSteamLibraries() {
  std::vector<fs::path> roots;
#if defined(_WIN32)
  for (const char* variable : {"ProgramFiles(x86)", "ProgramFiles"}) {
    const std::string base = GetEnvironment(variable);
    if (!base.empty()) {
      roots.push_back(fs::path(base) / "Steam");
    }
  }
#else
  const std::string home = GetEnvironment("HOME");
  if (!home.empty()) {
    roots.push_back(fs::path(home) / ".local/share/Steam");
    roots.push_back(fs::path(home) / ".steam/steam");
    roots.push_back(fs::path(home) / ".var/app/com.valvesoftware.Steam/.local/share/Steam");
  }
#endif
  std::vector<fs::path> libraries;
  auto add = [&libraries](const fs::path& path) {
    std::error_code error;
    if (!fs::is_directory(path / "steamapps", error)) {
      return;
    }
    fs::path key = fs::weakly_canonical(path, error);
    if (error) {
      key = path;
    }
    if (std::find(libraries.begin(), libraries.end(), key) == libraries.end()) {
      libraries.push_back(key);
    }
  };
  for (const fs::path& root : roots) {
    add(root);
    // Lines like: "path"  "D:\\SteamLibrary" (backslashes escaped).
    std::ifstream vdf(root / "steamapps" / "libraryfolders.vdf");
    std::string line;
    while (std::getline(vdf, line)) {
      const size_t key = line.find("\"path\"");
      if (key == std::string::npos) {
        continue;
      }
      const size_t open = line.find('"', key + 6);
      const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
      if (close == std::string::npos) {
        continue;
      }
      std::string value;
      for (size_t i = open + 1; i < close; ++i) {
        if (line[i] == '\\' && i + 1 < close) {
          ++i;
        }
        value.push_back(line[i]);
      }
      add(fs::path(value));
    }
  }
  return libraries;
}

fs::path FindLosslessDll() {
  std::error_code error;
  const std::string configured = REXCVAR_GET(present_lsfg_dll);
  if (!configured.empty()) {
    if (fs::is_regular_file(configured, error)) {
      return fs::path(configured);
    }
    REXLOG_WARN("LSFG: present_lsfg_dll \"{}\" is not a file", configured);
    return {};
  }
  for (const fs::path& library : GetSteamLibraries()) {
    const fs::path dll = library / "steamapps" / "common" / "Lossless Scaling" / "Lossless.dll";
    if (fs::is_regular_file(dll, error)) {
      return dll;
    }
  }
  return {};
}

#if REX_HAS_LSFG
// What the translated shaders need: SPIR-V 1.6 (Vulkan 1.3) with the Vulkan
// memory model, typeless storage images, and null descriptors for the inputs
// a pass leaves unbound.
std::string MissingFeature(const VulkanDevice& device) {
  const VulkanDevice::Properties& properties = device.properties();
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    return "needs Vulkan 1.3";
  }
  if (!properties.vulkanMemoryModel) {
    return "needs the Vulkan memory model";
  }
  if (!properties.shaderStorageImageReadWithoutFormat ||
      !properties.shaderStorageImageWriteWithoutFormat) {
    return "needs storage images without a format";
  }
  if (!properties.nullDescriptor) {
    return "needs null descriptors";
  }
  return {};
}
#endif

class VulkanLsfgFrameGenerator final : public VulkanFrameGenerator {
 public:
  explicit VulkanLsfgFrameGenerator(const VulkanDevice* device) : device_(device) {
    info_.id = "lsfg";
    info_.name = "Lossless Scaling frame generation";
    info_.version = "3.1";
    const fs::path dll = FindLosslessDll();
    if (dll.empty()) {
      info_.unavailable_reason = "no Lossless.dll found";
      return;
    }
#if REX_HAS_LSFG
    if (const std::string missing = MissingFeature(*device); !missing.empty()) {
      info_.unavailable_reason = missing;
      return;
    }
    resources_ = lsfg::ReadRcdataResources(dll);
    const bool performance = REXCVAR_GET(present_lsfg_performance);
    for (const std::string& name : lsfg::ShaderNames()) {
      if ((name.rfind("p_", 0) == 0) != performance) {
        continue;
      }
      if (!resources_.count(lsfg::ShaderResourceId(name))) {
        REXLOG_WARN("LSFG: {} has no shader {}", dll.string(), name);
        info_.unavailable_reason = "Lossless.dll is a version this build cannot read";
        resources_.clear();
        return;
      }
    }
    REXLOG_INFO("LSFG: shaders from {} ({} resources)", dll.string(), resources_.size());
    info_.available = true;
#else
    info_.unavailable_reason = "this build has no LSFG support";
#endif
  }

  const FrameGeneratorInfo& info() const override { return info_; }

  bool Prepare(uint32_t display_width, uint32_t display_height, uint32_t,
               uint32_t) override {
#if REX_HAS_LSFG
    if (!info_.available) {
      return false;
    }
    std::string error;
    if (!embedded_) {
      const VulkanInstance* instance = device_->vulkan_instance();
      lsfg::EmbeddedDevice embedded_device;
      embedded_device.get_instance_proc_addr = instance->functions().vkGetInstanceProcAddr;
      embedded_device.instance = instance->instance();
      embedded_device.get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
          embedded_device.get_instance_proc_addr(instance->instance(), "vkGetDeviceProcAddr"));
      embedded_device.device = device_->device();
      embedded_device.physical_device = device_->physical_device();
      embedded_device.queue_family = device_->queue_family_graphics_compute();
      lsfg::EmbeddedSettings settings;
      settings.performance = REXCVAR_GET(present_lsfg_performance);
      settings.flow_scale = float(REXCVAR_GET(present_lsfg_flow_scale));
      // Translated as the passes ask for them; the pipelines are kept.
      lsfg::ShaderSource source = [this](const std::string& name) {
        const auto it = resources_.find(lsfg::ShaderResourceId(name));
        if (it == resources_.end()) {
          return std::vector<uint8_t>();
        }
        try {
          return lsfg::TranslateShader(it->second);
        } catch (const std::exception& exception) {
          REXLOG_ERROR("LSFG: shader {} did not translate: {}", name, exception.what());
          return std::vector<uint8_t>();
        }
      };
      embedded_ = lsfg::Embedded::Create(embedded_device, std::move(source), settings, &error);
      if (!embedded_) {
        REXLOG_ERROR("LSFG: setup failed: {}", error);
        info_.available = false;
        info_.unavailable_reason = "setup failed";
        return false;
      }
    }
    if (!embedded_->Resize(display_width, display_height, &error)) {
      REXLOG_ERROR("LSFG: {}x{} failed: {}", display_width, display_height, error);
      return false;
    }
    return true;
#else
    (void)display_width;
    (void)display_height;
    return false;
#endif
  }

  bool Dispatch(VkCommandBuffer command_buffer,
                const VulkanFrameGeneratorInputs& inputs) override {
#if REX_HAS_LSFG
    if (!embedded_) {
      return false;
    }
    // A cut still goes into the history; its generated frame would blend
    // two unrelated pictures, so the real one is shown twice instead. The
    // presenter also marks the frame after any that generated nothing as a
    // reset, so only a reset that follows a generated frame is a cut: the
    // warm-up frames would otherwise keep it set for good.
    const bool cut = inputs.frame.reset && last_generated_;
    last_generated_ = embedded_->Record(command_buffer, inputs.present_color.image,
                                        inputs.output.image, !cut);
    return last_generated_;
#else
    (void)command_buffer;
    (void)inputs;
    return false;
#endif
  }

  void Release() override {
#if REX_HAS_LSFG
    embedded_.reset();
    last_generated_ = false;
#endif
  }

 private:
  const VulkanDevice* device_;
  FrameGeneratorInfo info_;
#if REX_HAS_LSFG
  std::map<uint32_t, std::vector<uint8_t>> resources_;
  std::unique_ptr<lsfg::Embedded> embedded_;
  bool last_generated_ = false;
#endif
};

}  // namespace

std::unique_ptr<VulkanFrameGenerator> CreateVulkanLsfgFrameGenerator(const VulkanDevice* device) {
  if (REXCVAR_GET(present_lsfg_dll).empty() && !REXCVAR_GET(present_lsfg_experimental)) {
    return nullptr;
  }
  return std::make_unique<VulkanLsfgFrameGenerator>(device);
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
