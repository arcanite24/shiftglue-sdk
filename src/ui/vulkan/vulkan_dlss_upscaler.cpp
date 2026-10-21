/**
 * DLSS Super Resolution and DLAA through NVIDIA NGX on Vulkan.
 *
 * Built with REXGLUE_DLSS_PROVIDER (cmake/rexglue_dlss.cmake, on by default
 * on 64-bit Windows), which downloads NVIDIA's NGX headers, static loader
 * and DLSS runtime at configure time; nothing of the DLSS SDK is committed.
 * The game stages nvngx_dlss.dll in upscalers/dlss/<version>/ next to the
 * executable, and NGX loads the newest version there (a player may add a
 * newer one) or the one present_dlss_runtime names. NGX needs a few NVIDIA instance and device
 * extensions, which instance and device creation ask for through
 * GetVulkanUpscaler*Extensions.
 */

#include <rex/ui/vulkan/upscaler.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
#include <nvsdk_ngx_helpers_vk.h>
#endif

REXCVAR_DEFINE_STRING(present_dlss_runtime, "", "UI/Presenter",
                      "Folder holding the DLSS runtime (nvngx_dlss.dll); empty searches "
                      "upscalers/dlss/<version> next to the executable, newest first");

REXCVAR_DEFINE_STRING(present_dlss_preset, "default", "UI/Presenter",
                      "DLSS preset: default (the DLSS runtime's choice per mode, and the one the "
                      "NVIDIA app's override replaces), j, k, l or m; e and f are the older "
                      "CNN model, deprecated by NVIDIA but cheaper")
    .allowed({"default", "e", "f", "j", "k", "l", "m"});

REXCVAR_DEFINE_INT32(present_dlss_jitter_sign, 0, "UI/Presenter",
                     "Debug: negate the jitter DLSS is given, 1 for x, 2 for y, 3 for both")
    .range(0, 3);

namespace rex {
namespace ui {
namespace vulkan {

#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;
// NGX identifies applications without an NVIDIA application id by a project
// id and the custom engine type.
constexpr const char* kProjectId = "6f1c2b3e-5a8d-4e7f-9c0b-2d4a6e8f1a3c";
constexpr const char* kEngineVersion = "1.0";

// Compares dotted versions ("310.10.0" after "310.9.16") number by number.
bool VersionLess(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    uint64_t x = 0, y = 0;
    while (i < a.size() && a[i] >= '0' && a[i] <= '9') x = x * 10 + uint64_t(a[i++] - '0');
    while (j < b.size() && b[j] >= '0' && b[j] <= '9') y = y * 10 + uint64_t(b[j++] - '0');
    if (x != y) {
      return x < y;
    }
    if (i < a.size()) ++i;
    if (j < b.size()) ++j;
  }
  return false;
}

void NVSDK_CONV LogNgxMessage(const char* message, NVSDK_NGX_Logging_Level,
                              NVSDK_NGX_Feature) {
  std::string text = message ? message : "";
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  // NGX logs every path it probes; the provider logs what matters.
  REXLOG_DEBUG("NGX: {}", text);
}

// What NGX is told about the application, shared by the extension queries
// and initialization: the folders to look for the DLSS runtime in, and
// where it may write its logs.
struct NgxSetup {
  std::vector<std::wstring> paths;
  std::vector<const wchar_t*> path_pointers;
  std::wstring data_path;
  NVSDK_NGX_FeatureCommonInfo common_info = {};
  NVSDK_NGX_FeatureDiscoveryInfo discovery = {};
  // Whether any folder holds a runtime.
  bool runtime_found = false;
};

const NgxSetup& GetNgxSetup() {
  static NgxSetup setup;
  static std::once_flag once;
  std::call_once(once, [] {
    constexpr const char* kRuntime = "nvngx_dlss.dll";
    std::vector<std::filesystem::path> folders;
    const std::string configured = REXCVAR_GET(present_dlss_runtime);
    if (!configured.empty()) {
      folders.emplace_back(configured);
    } else {
      const std::filesystem::path provider_folder =
          rex::filesystem::GetExecutableFolder() / "upscalers" / "dlss";
      std::vector<std::filesystem::path> versions;
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator(provider_folder, error)) {
        if (entry.is_directory(error)) {
          versions.push_back(entry.path());
        }
      }
      std::sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) {
        return VersionLess(b.filename().string(), a.filename().string());
      });
      folders = std::move(versions);
      folders.push_back(provider_folder);
    }
    for (const std::filesystem::path& folder : folders) {
      std::error_code error;
      if (std::filesystem::is_regular_file(folder / kRuntime, error)) {
        setup.runtime_found = true;
        setup.paths.push_back(folder.wstring());
      }
    }
    for (const std::wstring& path : setup.paths) {
      setup.path_pointers.push_back(path.c_str());
    }
    std::error_code error;
    std::filesystem::path data_path = std::filesystem::temp_directory_path(error) / "rexglue-ngx";
    std::filesystem::create_directories(data_path, error);
    setup.data_path = data_path.wstring();
    setup.common_info.PathListInfo.Path = setup.path_pointers.data();
    setup.common_info.PathListInfo.Length = uint32_t(setup.path_pointers.size());
    setup.common_info.LoggingInfo.LoggingCallback = LogNgxMessage;
    setup.common_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    setup.common_info.LoggingInfo.DisableOtherLoggingSinks = true;
    setup.discovery.SDKVersion = NVSDK_NGX_Version_API;
    setup.discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    setup.discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    setup.discovery.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    setup.discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    setup.discovery.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    setup.discovery.ApplicationDataPath = setup.data_path.c_str();
    setup.discovery.FeatureInfo = &setup.common_info;
    if (setup.runtime_found) {
      REXLOG_INFO("DLSS runtime folder: {}", std::filesystem::path(setup.paths[0]).string());
    } else {
      REXLOG_INFO("DLSS runtime unavailable: {} not found in upscalers/dlss", kRuntime);
    }
  });
  return setup;
}

// Extension names NGX asked for, kept for the life of the process.
std::deque<std::string> g_extension_names;
std::mutex g_extension_names_mutex;

std::vector<const char*> KeepExtensionNames(const VkExtensionProperties* properties,
                                            uint32_t count) {
  std::lock_guard<std::mutex> lock(g_extension_names_mutex);
  std::vector<const char*> names;
  for (uint32_t i = 0; properties && i < count; ++i) {
    g_extension_names.emplace_back(properties[i].extensionName);
    names.push_back(g_extension_names.back().c_str());
  }
  return names;
}

NVSDK_NGX_PerfQuality_Value PerfQualityForRatio(float ratio) {
  if (ratio <= 1.01f) return NVSDK_NGX_PerfQuality_Value_DLAA;
  if (ratio <= 1.55f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
  if (ratio <= 1.8f) return NVSDK_NGX_PerfQuality_Value_Balanced;
  if (ratio <= 2.5f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
  return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

NVSDK_NGX_DLSS_Hint_Render_Preset PresetFromCvar() {
  const std::string preset = REXCVAR_GET(present_dlss_preset);
  if (preset == "e") return NVSDK_NGX_DLSS_Hint_Render_Preset_E;
  if (preset == "f") return NVSDK_NGX_DLSS_Hint_Render_Preset_F;
  if (preset == "j") return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
  if (preset == "k") return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
  if (preset == "l") return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
  if (preset == "m") return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
  return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
}

class VulkanDlssUpscaler final : public VulkanUpscaler {
 public:
  explicit VulkanDlssUpscaler(const VulkanDevice* device) : device_(device) {
    info_.id = "dlss";
    info_.name = "NVIDIA DLSS Super Resolution";
    info_.kind = UpscalerKind::kTemporal;
    info_.mode_mask = (1u << uint32_t(UpscalerMode::kCount)) - 1;
    info_.uses_depth = true;
    info_.uses_motion_vectors = true;
    info_.uses_jitter = true;
    if (device->properties().vendorID != kNvidiaVendorId) {
      info_.unavailable_reason = "needs an NVIDIA RTX GPU";
      return;
    }
    const NgxSetup& setup = GetNgxSetup();
    if (!setup.runtime_found) {
      info_.unavailable_reason = "no nvngx_dlss.dll in upscalers/dlss";
      return;
    }
    const VulkanInstance* instance = device->vulkan_instance();
    const PFN_vkGetInstanceProcAddr get_instance_proc_addr =
        instance->functions().vkGetInstanceProcAddr;
    const auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance_proc_addr(instance->instance(), "vkGetDeviceProcAddr"));
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, setup.data_path.c_str(),
        instance->instance(), device->physical_device(), device->device(), get_instance_proc_addr,
        get_device_proc_addr, &setup.common_info);
    if (NVSDK_NGX_FAILED(result)) {
      info_.unavailable_reason = "NGX did not start (" + std::to_string(uint32_t(result)) + ")";
      REXLOG_WARN("DLSS: NGX initialization failed with 0x{:08X}", uint32_t(result));
      return;
    }
    initialized_ = true;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters_)) ||
        !parameters_) {
      info_.unavailable_reason = "NGX has no capability parameters";
      return;
    }
    int available = 0;
    NVSDK_NGX_Parameter_GetI(parameters_, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) {
      int needs_driver = 0;
      NVSDK_NGX_Parameter_GetI(parameters_, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                               &needs_driver);
      int init_result = 0;
      NVSDK_NGX_Parameter_GetI(parameters_, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
                               &init_result);
      info_.unavailable_reason = needs_driver ? "needs a newer NVIDIA driver"
                                              : "DLSS is not supported on this GPU";
      REXLOG_INFO("DLSS unavailable: {} (feature init result 0x{:08X})", info_.unavailable_reason,
                  uint32_t(init_result));
      return;
    }
    info_.available = true;
    // The runtime folders are named after the DLSS version.
    info_.version = std::filesystem::path(setup.paths[0]).filename().string();
    REXLOG_INFO("DLSS available on {}", device->properties().deviceName);
  }

  ~VulkanDlssUpscaler() override {
    for (uint32_t i = 0; i < kHistoryCount; ++i) {
      SelectHistory(i);
      Release();
    }
    if (parameters_) {
      NVSDK_NGX_VULKAN_DestroyParameters(parameters_);
      parameters_ = nullptr;
    }
    if (initialized_) {
      NVSDK_NGX_VULKAN_Shutdown1(device_->device());
    }
  }

  const UpscalerInfo& info() const override { return info_; }

  bool Prepare(uint32_t max_render_width, uint32_t max_render_height, uint32_t max_output_width,
               uint32_t max_output_height, bool hdr) override {
    if (!info_.available) {
      return false;
    }
    const NVSDK_NGX_DLSS_Hint_Render_Preset preset = PresetFromCvar();
    History& history = h();
    if (history.feature &&
        (max_render_width != history.render_width ||
         max_render_height != history.render_height ||
         max_output_width != history.output_width ||
         max_output_height != history.output_height || hdr != history.hdr ||
         preset != history.preset)) {
      Release();
    }
    h().render_width = max_render_width;
    h().render_height = max_render_height;
    h().output_width = max_output_width;
    h().output_height = max_output_height;
    h().hdr = hdr;
    h().preset = preset;
    return true;
  }

  bool Dispatch(VkCommandBuffer command_buffer, const VulkanUpscalerInputs& inputs) override {
    if (!info_.available || !inputs.color.image || !inputs.output.image ||
        !inputs.depth.image || !inputs.motion_vectors.image) {
      return false;
    }
    const UpscalerFrame& frame = inputs.frame;
    if (!h().feature && !CreateFeature(command_buffer, frame)) {
      return false;
    }
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    NVSDK_NGX_Resource_VK color = NVSDK_NGX_Create_ImageView_Resource_VK(
        inputs.color.view, inputs.color.image, range, inputs.color.format, inputs.color.width,
        inputs.color.height, false);
    VkImageSubresourceRange depth_range = range;
    if (inputs.depth.format == VK_FORMAT_D32_SFLOAT ||
        inputs.depth.format == VK_FORMAT_D16_UNORM ||
        inputs.depth.format == VK_FORMAT_D24_UNORM_S8_UINT ||
        inputs.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
      depth_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    NVSDK_NGX_Resource_VK depth = NVSDK_NGX_Create_ImageView_Resource_VK(
        inputs.depth.view, inputs.depth.image, depth_range, inputs.depth.format,
        inputs.depth.width, inputs.depth.height, false);
    NVSDK_NGX_Resource_VK motion_vectors = NVSDK_NGX_Create_ImageView_Resource_VK(
        inputs.motion_vectors.view, inputs.motion_vectors.image, range,
        inputs.motion_vectors.format, inputs.motion_vectors.width, inputs.motion_vectors.height,
        false);
    NVSDK_NGX_Resource_VK output = NVSDK_NGX_Create_ImageView_Resource_VK(
        inputs.output.view, inputs.output.image, range, inputs.output.format, inputs.output.width,
        inputs.output.height, true);

    const int32_t jitter_sign = REXCVAR_GET(present_dlss_jitter_sign);
    NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion_vectors;
    eval.InJitterOffsetX = (jitter_sign & 1) ? -frame.jitter_x : frame.jitter_x;
    eval.InJitterOffsetY = (jitter_sign & 2) ? -frame.jitter_y : frame.jitter_y;
    eval.InRenderSubrectDimensions.Width = frame.render_width;
    eval.InRenderSubrectDimensions.Height = frame.render_height;
    eval.InReset = frame.reset ? 1 : 0;
    eval.InMVScaleX = frame.motion_vector_scale_x;
    eval.InMVScaleY = frame.motion_vector_scale_y;
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;
    eval.InFrameTimeDeltaInMsec = frame.frame_delta_ms;
    const NVSDK_NGX_Result result =
        NGX_VULKAN_EVALUATE_DLSS_EXT(command_buffer, h().feature, parameters_, &eval);
    if (NVSDK_NGX_FAILED(result)) {
      if (!evaluate_failure_logged_) {
        evaluate_failure_logged_ = true;
        REXLOG_WARN("DLSS: evaluation failed with 0x{:08X}", uint32_t(result));
      }
      return false;
    }
    return true;
  }

  void Release() override {
    if (h().feature) {
      NVSDK_NGX_VULKAN_ReleaseFeature(h().feature);
      h().feature = nullptr;
    }
  }

  uint32_t history_count() const override { return kHistoryCount; }
  void SelectHistory(uint32_t index) override { history_ = index < kHistoryCount ? index : 0; }

 private:
  bool CreateFeature(VkCommandBuffer command_buffer, const UpscalerFrame& frame) {
    const uint32_t render_width = std::max(h().render_width, frame.render_width);
    const uint32_t render_height = std::max(h().render_height, frame.render_height);
    const uint32_t output_width = std::max(h().output_width, frame.output_width);
    const uint32_t output_height = std::max(h().output_height, frame.output_height);
    if (!render_width || !render_height || !output_width || !output_height) {
      return false;
    }
    const NVSDK_NGX_PerfQuality_Value quality =
        PerfQualityForRatio(float(output_width) / float(render_width));
    for (const char* name : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
      NVSDK_NGX_Parameter_SetUI(parameters_, name, uint32_t(h().preset));
    }
    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = render_width;
    create.Feature.InHeight = render_height;
    create.Feature.InTargetWidth = output_width;
    create.Feature.InTargetHeight = output_height;
    create.Feature.InPerfQualityValue = quality;
    // Motion vectors are in render pixels without the jitter; depth is 0
    // near unless the frame says otherwise.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (frame.depth_inverted) {
      create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    }
    if (h().hdr) {
      create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    }
    const NVSDK_NGX_Result result =
        NGX_VULKAN_CREATE_DLSS_EXT(command_buffer, 1, 1, &h().feature, parameters_, &create);
    if (NVSDK_NGX_FAILED(result) || !h().feature) {
      h().feature = nullptr;
      if (!create_failure_logged_) {
        create_failure_logged_ = true;
        REXLOG_WARN("DLSS: could not create the feature for {}x{} to {}x{} (0x{:08X})",
                    render_width, render_height, output_width, output_height, uint32_t(result));
      }
      return false;
    }
    h().depth_inverted = frame.depth_inverted;
    REXLOG_INFO("DLSS: feature for {}x{} to {}x{}, quality {}, preset {}{}", render_width,
                render_height, output_width, output_height, int(quality), int(h().preset),
                h().hdr ? ", HDR" : "");
    return true;
  }

  const VulkanDevice* device_;
  UpscalerInfo info_;
  bool initialized_ = false;
  NVSDK_NGX_Parameter* parameters_ = nullptr;
  // One DLSS feature per history: the presenter's (0) and, in VR, one per
  // eye, each with its own frames to accumulate.
  struct History {
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    bool hdr = false;
    bool depth_inverted = false;
    NVSDK_NGX_DLSS_Hint_Render_Preset preset = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
  };
  static constexpr uint32_t kHistoryCount = 3;
  History histories_[kHistoryCount];
  uint32_t history_ = 0;
  History& h() { return histories_[history_]; }
  bool create_failure_logged_ = false;
  bool evaluate_failure_logged_ = false;
};

}  // namespace

std::vector<const char*> GetVulkanUpscalerInstanceExtensions() {
  const NgxSetup& setup = GetNgxSetup();
  if (!setup.runtime_found) {
    return {};
  }
  uint32_t count = 0;
  VkExtensionProperties* properties = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
          &setup.discovery, &count, &properties))) {
    return {};
  }
  return KeepExtensionNames(properties, count);
}

std::vector<const char*> GetVulkanUpscalerDeviceExtensions(VkInstance instance,
                                                           VkPhysicalDevice physical_device,
                                                           uint32_t vendor_id) {
  if (vendor_id != kNvidiaVendorId) {
    return {};
  }
  const NgxSetup& setup = GetNgxSetup();
  if (!setup.runtime_found) {
    return {};
  }
  uint32_t count = 0;
  VkExtensionProperties* properties = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
          instance, physical_device, &setup.discovery, &count, &properties))) {
    return {};
  }
  return KeepExtensionNames(properties, count);
}

std::unique_ptr<VulkanUpscaler> CreateVulkanDlssUpscaler(const VulkanDevice* device) {
  return std::make_unique<VulkanDlssUpscaler>(device);
}

#else

std::vector<const char*> GetVulkanUpscalerInstanceExtensions() { return {}; }

std::vector<const char*> GetVulkanUpscalerDeviceExtensions(VkInstance, VkPhysicalDevice,
                                                           uint32_t) {
  return {};
}

std::unique_ptr<VulkanUpscaler> CreateVulkanDlssUpscaler(const VulkanDevice*) { return nullptr; }

#endif  // defined(REX_HAS_DLSS) && REX_HAS_DLSS

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
