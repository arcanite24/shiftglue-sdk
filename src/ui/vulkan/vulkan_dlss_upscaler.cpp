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
#include <rex/ui/vulkan/frame_generator.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

REXCVAR_DEFINE_STRING(present_dlss_runtime, "", "UI/Presenter",
                      "Folder holding the DLSS runtime (nvngx_dlss.dll); empty searches "
                      "upscalers/dlss/<version> next to the executable, newest first");

REXCVAR_DEFINE_STRING(present_dlss_preset, "default", "UI/Presenter",
                      "DLSS preset: default (the DLSS runtime's choice per mode, and the one the "
                      "NVIDIA app's override replaces), j, k, l or m")
    .allowed({"default", "j", "k", "l", "m"});

REXCVAR_DEFINE_BOOL(present_dlss_frame_generation, true, "UI/Presenter",
                    "Offer DLSS frame generation (present_frame_generation=dlss) through NGX; "
                    "needs nvngx_dlssg.dll beside nvngx_dlss.dll and an RTX 40 or newer GPU")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

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

// NGX is started once per device and shut down after its last user (DLSS
// Super Resolution and DLSS frame generation share it).
std::mutex g_ngx_mutex;
uint32_t g_ngx_users = 0;
VkDevice g_ngx_device = VK_NULL_HANDLE;

NVSDK_NGX_Result AcquireNgx(const VulkanDevice* device) {
  std::lock_guard<std::mutex> lock(g_ngx_mutex);
  if (g_ngx_users) {
    if (g_ngx_device != device->device()) {
      return NVSDK_NGX_Result_Fail;
    }
    ++g_ngx_users;
    return NVSDK_NGX_Result_Success;
  }
  const NgxSetup& setup = GetNgxSetup();
  const VulkanInstance* instance = device->vulkan_instance();
  const PFN_vkGetInstanceProcAddr get_instance_proc_addr =
      instance->functions().vkGetInstanceProcAddr;
  const auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      get_instance_proc_addr(instance->instance(), "vkGetDeviceProcAddr"));
  const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
      kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, setup.data_path.c_str(),
      instance->instance(), device->physical_device(), device->device(), get_instance_proc_addr,
      get_device_proc_addr, &setup.common_info);
  if (!NVSDK_NGX_FAILED(result)) {
    g_ngx_users = 1;
    g_ngx_device = device->device();
  }
  return result;
}

void ReleaseNgx() {
  std::lock_guard<std::mutex> lock(g_ngx_mutex);
  if (g_ngx_users && !--g_ngx_users) {
    NVSDK_NGX_VULKAN_Shutdown1(g_ngx_device);
    g_ngx_device = VK_NULL_HANDLE;
  }
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
    NVSDK_NGX_Result result = AcquireNgx(device);
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
    Release();
    if (parameters_) {
      NVSDK_NGX_VULKAN_DestroyParameters(parameters_);
      parameters_ = nullptr;
    }
    if (initialized_) {
      ReleaseNgx();
    }
  }

  const UpscalerInfo& info() const override { return info_; }

  bool Prepare(uint32_t max_render_width, uint32_t max_render_height, uint32_t max_output_width,
               uint32_t max_output_height, bool hdr) override {
    if (!info_.available) {
      return false;
    }
    const NVSDK_NGX_DLSS_Hint_Render_Preset preset = PresetFromCvar();
    if (feature_ && (max_render_width != render_width_ || max_render_height != render_height_ ||
                     max_output_width != output_width_ || max_output_height != output_height_ ||
                     hdr != hdr_ || preset != preset_)) {
      Release();
    }
    render_width_ = max_render_width;
    render_height_ = max_render_height;
    output_width_ = max_output_width;
    output_height_ = max_output_height;
    hdr_ = hdr;
    preset_ = preset;
    return true;
  }

  bool Dispatch(VkCommandBuffer command_buffer, const VulkanUpscalerInputs& inputs) override {
    if (!info_.available || !inputs.color.image || !inputs.output.image ||
        !inputs.depth.image || !inputs.motion_vectors.image) {
      return false;
    }
    const UpscalerFrame& frame = inputs.frame;
    if (!feature_ && !CreateFeature(command_buffer, frame)) {
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
        NGX_VULKAN_EVALUATE_DLSS_EXT(command_buffer, feature_, parameters_, &eval);
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
    if (feature_) {
      NVSDK_NGX_VULKAN_ReleaseFeature(feature_);
      feature_ = nullptr;
    }
  }

 private:
  bool CreateFeature(VkCommandBuffer command_buffer, const UpscalerFrame& frame) {
    const uint32_t render_width = std::max(render_width_, frame.render_width);
    const uint32_t render_height = std::max(render_height_, frame.render_height);
    const uint32_t output_width = std::max(output_width_, frame.output_width);
    const uint32_t output_height = std::max(output_height_, frame.output_height);
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
      NVSDK_NGX_Parameter_SetUI(parameters_, name, uint32_t(preset_));
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
    if (hdr_) {
      create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    }
    const NVSDK_NGX_Result result =
        NGX_VULKAN_CREATE_DLSS_EXT(command_buffer, 1, 1, &feature_, parameters_, &create);
    if (NVSDK_NGX_FAILED(result) || !feature_) {
      feature_ = nullptr;
      if (!create_failure_logged_) {
        create_failure_logged_ = true;
        REXLOG_WARN("DLSS: could not create the feature for {}x{} to {}x{} (0x{:08X})",
                    render_width, render_height, output_width, output_height, uint32_t(result));
      }
      return false;
    }
    depth_inverted_ = frame.depth_inverted;
    REXLOG_INFO("DLSS: feature for {}x{} to {}x{}, quality {}, preset {}{}", render_width,
                render_height, output_width, output_height, int(quality), int(preset_),
                hdr_ ? ", HDR" : "");
    return true;
  }

  const VulkanDevice* device_;
  UpscalerInfo info_;
  bool initialized_ = false;
  NVSDK_NGX_Parameter* parameters_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  uint32_t render_width_ = 0;
  uint32_t render_height_ = 0;
  uint32_t output_width_ = 0;
  uint32_t output_height_ = 0;
  bool hdr_ = false;
  bool depth_inverted_ = false;
  NVSDK_NGX_DLSS_Hint_Render_Preset preset_ = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
  bool create_failure_logged_ = false;
  bool evaluate_failure_logged_ = false;
};

// UP-4.3: DLSS frame generation through NGX, driven by the presenter like
// FSR 3 frame generation (first run on an RTX 4080, 2026-11-16). The camera
// matrices are built from the field of view; the motion vectors include the
// camera's motion.
class VulkanDlssFrameGenerator final : public VulkanFrameGenerator {
 public:
  explicit VulkanDlssFrameGenerator(const VulkanDevice* device) : device_(device) {
    info_.id = "dlss";
    info_.name = "NVIDIA DLSS frame generation";
    info_.uses_depth = true;
    info_.uses_motion_vectors = true;
    if (device->properties().vendorID != kNvidiaVendorId) {
      info_.unavailable_reason = "needs an NVIDIA RTX 40 or newer GPU";
      return;
    }
    const NgxSetup& setup = GetNgxSetup();
    std::error_code error;
    if (!setup.runtime_found ||
        !std::filesystem::is_regular_file(
            std::filesystem::path(setup.paths[0]) / "nvngx_dlssg.dll", error)) {
      info_.unavailable_reason = "no nvngx_dlssg.dll beside nvngx_dlss.dll";
      return;
    }
    if (NVSDK_NGX_FAILED(AcquireNgx(device))) {
      info_.unavailable_reason = "NGX did not start";
      return;
    }
    initialized_ = true;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters_)) ||
        !parameters_) {
      info_.unavailable_reason = "NGX has no capability parameters";
      return;
    }
    int available = 0;
    NVSDK_NGX_Parameter_GetI(parameters_, NVSDK_NGX_Parameter_FrameGeneration_Available,
                             &available);
    if (!available) {
      info_.unavailable_reason = "DLSS frame generation is not supported on this GPU";
      return;
    }
    info_.version = std::filesystem::path(setup.paths[0]).filename().string();
    info_.available = true;
    REXLOG_INFO("DLSS frame generation available");
  }

  ~VulkanDlssFrameGenerator() override {
    Release();
    if (parameters_) {
      NVSDK_NGX_VULKAN_DestroyParameters(parameters_);
      parameters_ = nullptr;
    }
    if (initialized_) {
      ReleaseNgx();
    }
  }

  const FrameGeneratorInfo& info() const override { return info_; }

  bool Prepare(uint32_t display_width, uint32_t display_height, uint32_t max_render_width,
               uint32_t max_render_height) override {
    if (!info_.available) {
      return false;
    }
    if (feature_ && (display_width != display_width_ || display_height != display_height_ ||
                     max_render_width != render_width_ || max_render_height != render_height_)) {
      Release();
    }
    display_width_ = display_width;
    display_height_ = display_height;
    render_width_ = max_render_width;
    render_height_ = max_render_height;
    return true;
  }

  bool Dispatch(VkCommandBuffer command_buffer,
                const VulkanFrameGeneratorInputs& inputs) override {
    if (!info_.available || !inputs.present_color.image || !inputs.output.image ||
        !inputs.depth.image || !inputs.motion_vectors.image) {
      return false;
    }
    const UpscalerFrame& frame = inputs.frame;
    if (!feature_) {
      NVSDK_NGX_DLSSG_Create_Params create = {};
      create.Width = display_width_;
      create.Height = display_height_;
      create.NativeBackbufferFormat = uint32_t(inputs.present_color.format);
      create.RenderWidth = render_width_;
      create.RenderHeight = render_height_;
      const NVSDK_NGX_Result result =
          NGX_VK_CREATE_DLSSG(command_buffer, 1, 1, &feature_, parameters_, &create);
      if (NVSDK_NGX_FAILED(result) || !feature_) {
        feature_ = nullptr;
        if (!failure_logged_) {
          failure_logged_ = true;
          REXLOG_WARN("DLSS frame generation: could not create the feature (0x{:08X})",
                      uint32_t(result));
        }
        return false;
      }
      REXLOG_INFO("DLSS frame generation: feature for {}x{}, render {}x{}", display_width_,
                  display_height_, render_width_, render_height_);
    }
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto resource = [&range](const VulkanUpscalerImage& image, bool writable) {
      return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, image.format,
                                                    image.width, image.height, writable);
    };
    NVSDK_NGX_Resource_VK backbuffer = resource(inputs.present_color, false);
    NVSDK_NGX_Resource_VK depth = resource(inputs.depth, false);
    NVSDK_NGX_Resource_VK motion_vectors = resource(inputs.motion_vectors, false);
    NVSDK_NGX_Resource_VK output = resource(inputs.output, true);
    NVSDK_NGX_VK_DLSSG_Eval_Params eval = {};
    eval.pBackbuffer = &backbuffer;
    eval.pDepth = &depth;
    eval.pMVecs = &motion_vectors;
    eval.pOutputInterpFrame = &output;

    NVSDK_NGX_DLSSG_Opt_Eval_Params options = {};
    // A perspective projection from the field of view (depth 0 near, as the
    // game's), its inverse, and no reprojection: the motion vectors carry
    // the camera's motion.
    const float aspect =
        frame.render_height ? float(frame.render_width) / float(frame.render_height) : 1.0f;
    const float y_scale = 1.0f / std::tan(std::max(frame.camera_fov_y, 0.01f) * 0.5f);
    const float x_scale = y_scale / aspect;
    const float near_plane = frame.camera_near;
    const float far_plane = std::max(frame.camera_far, near_plane + 0.001f);
    const float z_scale = far_plane / (far_plane - near_plane);
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        options.cameraViewToClip[r][c] = 0.0f;
        options.clipToCameraView[r][c] = 0.0f;
        options.clipToLensClip[r][c] = r == c ? 1.0f : 0.0f;
        options.clipToPrevClip[r][c] = r == c ? 1.0f : 0.0f;
        options.prevClipToClip[r][c] = r == c ? 1.0f : 0.0f;
      }
    }
    options.cameraViewToClip[0][0] = x_scale;
    options.cameraViewToClip[1][1] = y_scale;
    options.cameraViewToClip[2][2] = z_scale;
    options.cameraViewToClip[2][3] = 1.0f;
    options.cameraViewToClip[3][2] = -near_plane * z_scale;
    options.clipToCameraView[0][0] = 1.0f / x_scale;
    options.clipToCameraView[1][1] = 1.0f / y_scale;
    options.clipToCameraView[2][3] = -1.0f / (near_plane * z_scale);
    options.clipToCameraView[3][2] = 1.0f;
    options.clipToCameraView[3][3] = 1.0f / near_plane;
    options.jitterOffset[0] = frame.jitter_x;
    options.jitterOffset[1] = frame.jitter_y;
    // As for DLSS Super Resolution.
    options.mvecScale[0] = frame.motion_vector_scale_x;
    options.mvecScale[1] = frame.motion_vector_scale_y;
    options.cameraUp[1] = 1.0f;
    options.cameraRight[0] = 1.0f;
    options.cameraFwd[2] = 1.0f;
    options.cameraNear = near_plane;
    options.cameraFar = far_plane;
    options.cameraFOV = frame.camera_fov_y;
    options.cameraAspectRatio = aspect;
    options.colorBuffersHDR = false;
    options.depthInverted = frame.depth_inverted;
    options.cameraMotionIncluded = true;
    options.reset = frame.reset;
    options.mvecsSubrectSize = {frame.render_width, frame.render_height};
    options.depthSubrectSize = {frame.render_width, frame.render_height};
    options.backbufferSubrectBase = {uint32_t(inputs.generation_x),
                                     uint32_t(inputs.generation_y)};
    options.backbufferSubrectSize = {inputs.generation_width, inputs.generation_height};
    options.outputInterpSubrectBase = options.backbufferSubrectBase;
    options.outputInterpSubrectSize = options.backbufferSubrectSize;
    const NVSDK_NGX_Result result =
        NGX_VK_EVALUATE_DLSSG(command_buffer, feature_, parameters_, &eval, &options);
    if (NVSDK_NGX_FAILED(result)) {
      if (!failure_logged_) {
        failure_logged_ = true;
        REXLOG_WARN("DLSS frame generation: evaluation failed with 0x{:08X}", uint32_t(result));
      }
      return false;
    }
    return true;
  }

  void Release() override {
    if (feature_) {
      NVSDK_NGX_VULKAN_ReleaseFeature(feature_);
      feature_ = nullptr;
    }
  }

 private:
  const VulkanDevice* device_;
  FrameGeneratorInfo info_;
  bool initialized_ = false;
  NVSDK_NGX_Parameter* parameters_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  uint32_t display_width_ = 0;
  uint32_t display_height_ = 0;
  uint32_t render_width_ = 0;
  uint32_t render_height_ = 0;
  bool failure_logged_ = false;
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
  std::vector<const char*> names = KeepExtensionNames(properties, count);
  if (REXCVAR_GET(present_dlss_frame_generation)) {
    NVSDK_NGX_FeatureDiscoveryInfo discovery = setup.discovery;
    discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    count = 0;
    properties = nullptr;
    if (!NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &discovery, &count, &properties))) {
      for (const char* name : KeepExtensionNames(properties, count)) {
        if (std::none_of(names.begin(), names.end(),
                         [name](const char* other) { return !std::strcmp(name, other); })) {
          names.push_back(name);
        }
      }
    }
  }
  return names;
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
  std::vector<const char*> names = KeepExtensionNames(properties, count);
  if (REXCVAR_GET(present_dlss_frame_generation)) {
    NVSDK_NGX_FeatureDiscoveryInfo discovery = setup.discovery;
    discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    count = 0;
    properties = nullptr;
    if (!NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            instance, physical_device, &discovery, &count, &properties))) {
      for (const char* name : KeepExtensionNames(properties, count)) {
        if (std::none_of(names.begin(), names.end(),
                         [name](const char* other) { return !std::strcmp(name, other); })) {
          names.push_back(name);
        }
      }
    }
  }
  return names;
}

std::unique_ptr<VulkanUpscaler> CreateVulkanDlssUpscaler(const VulkanDevice* device) {
  return std::make_unique<VulkanDlssUpscaler>(device);
}

std::unique_ptr<VulkanFrameGenerator> CreateVulkanDlssFrameGenerator(const VulkanDevice* device) {
  if (!REXCVAR_GET(present_dlss_frame_generation)) {
    return nullptr;
  }
  return std::make_unique<VulkanDlssFrameGenerator>(device);
}

#else

std::vector<const char*> GetVulkanUpscalerInstanceExtensions() { return {}; }

std::vector<const char*> GetVulkanUpscalerDeviceExtensions(VkInstance, VkPhysicalDevice,
                                                           uint32_t) {
  return {};
}

std::unique_ptr<VulkanUpscaler> CreateVulkanDlssUpscaler(const VulkanDevice*) { return nullptr; }

std::unique_ptr<VulkanFrameGenerator> CreateVulkanDlssFrameGenerator(const VulkanDevice*) {
  return nullptr;
}

#endif  // defined(REX_HAS_DLSS) && REX_HAS_DLSS

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
