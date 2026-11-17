/**
 * FSR 2 and FSR 3 temporal upscalers through the FidelityFX API.
 *
 * The API is five C entry points. They come from the SDK when it is built
 * from source (REX_HAS_FIDELITYFX_RUNTIME), otherwise from AMD's signed
 * runtime, amd_fidelityfx_vk.dll, loaded at run time from the folder setup
 * fetches it to (docs/LEGAL.md in Pinyon Shift: nothing vendor-made is
 * committed or shipped). One runtime holds several upscaler versions (1.1.4:
 * FSR 3.1.4 and 2.3.3); each provider here pins one through the API's version
 * override.
 */

#include <rex/ui/vulkan/frame_generator.h>
#include <rex/ui/vulkan/upscaler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

#if defined(REX_HAS_FFX_API) && REX_HAS_FFX_API
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

REXCVAR_DEFINE_STRING(present_ffx_runtime, "", "UI/Presenter",
                      "Path to the FidelityFX runtime (amd_fidelityfx_vk.dll, or "
                      "libamd_fidelityfx_vk.so/.dylib) for FSR 2 and 3; empty searches "
                      "upscalers/fidelityfx/<version> next to the executable, newest first "
                      "(and the package's libraries on Android)");

REXCVAR_DEFINE_BOOL(present_ffx_frame_generation_debug, false, "UI/Presenter",
                    "Debug: FSR 3 frame generation draws its debug view (its inputs and "
                    "what it made of them) into the generated frames")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex {
namespace ui {
namespace vulkan {

#if defined(REX_HAS_FFX_API) && REX_HAS_FFX_API
namespace {

struct FfxRuntime {
  PfnFfxCreateContext create_context = nullptr;
  PfnFfxDestroyContext destroy_context = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
  PfnFfxConfigure configure = nullptr;
  std::string source;
  std::string error;
  bool loaded() const {
    return create_context && destroy_context && query && dispatch && configure;
  }
};

// Compares dotted versions ("1.1.10" after "1.1.4") number by number.
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

const FfxRuntime& GetFfxRuntime() {
  static FfxRuntime runtime;
  static std::once_flag once;
  std::call_once(once, [] {
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
    runtime.create_context = &ffxCreateContext;
    runtime.destroy_context = &ffxDestroyContext;
    runtime.query = &ffxQuery;
    runtime.dispatch = &ffxDispatch;
    runtime.configure = &ffxConfigure;
    runtime.source = "linked";
#else
#if REX_PLATFORM_WIN32
    const char* const kName = "amd_fidelityfx_vk.dll";
#elif defined(__APPLE__)
    const char* const kName = "libamd_fidelityfx_vk.dylib";
#else
    const char* const kName = "libamd_fidelityfx_vk.so";
#endif
    std::vector<std::filesystem::path> candidates;
    const std::string configured = REXCVAR_GET(present_ffx_runtime);
    if (!configured.empty()) {
      candidates.emplace_back(configured);
    } else {
      // upscalers/fidelityfx/<version>/, newest version first, then the
      // provider folder itself and the executable's folder.
      const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
      const std::filesystem::path provider_folder = folder / "upscalers" / "fidelityfx";
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
      for (const std::filesystem::path& version : versions) {
        candidates.push_back(version / kName);
      }
      candidates.push_back(provider_folder / kName);
      candidates.push_back(folder / kName);
#if defined(__ANDROID__)
      // The package's native library folder, which the linker searches for
      // a bare name (thirdparty/fidelityfx-vk builds it for arm64).
      candidates.emplace_back(kName);
#endif
    }
    for (const std::filesystem::path& path : candidates) {
      std::error_code error;
      if (path.has_parent_path() && !std::filesystem::is_regular_file(path, error)) {
        continue;
      }
#if REX_PLATFORM_WIN32
      HMODULE module = LoadLibraryW(path.wstring().c_str());
      auto symbol = [module](const char* name) {
        return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
      };
#else
      void* module = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
      auto symbol = [module](const char* name) { return module ? dlsym(module, name) : nullptr; };
#endif
      if (!module) {
        runtime.error = "could not load " + path.string();
        continue;
      }
      runtime.create_context = reinterpret_cast<PfnFfxCreateContext>(symbol("ffxCreateContext"));
      runtime.destroy_context =
          reinterpret_cast<PfnFfxDestroyContext>(symbol("ffxDestroyContext"));
      runtime.query = reinterpret_cast<PfnFfxQuery>(symbol("ffxQuery"));
      runtime.dispatch = reinterpret_cast<PfnFfxDispatch>(symbol("ffxDispatch"));
      runtime.configure = reinterpret_cast<PfnFfxConfigure>(symbol("ffxConfigure"));
      if (runtime.loaded()) {
        runtime.source = path.string();
        runtime.error.clear();
        break;
      }
      runtime.error = path.string() + " lacks the FidelityFX API entry points";
    }
    if (!runtime.loaded() && runtime.error.empty()) {
      runtime.error = std::string(kName) + " not found";
    }
#endif
    if (runtime.loaded()) {
      REXLOG_INFO("FidelityFX runtime: {}", runtime.source);
    } else {
      REXLOG_INFO("FidelityFX runtime unavailable: {}", runtime.error);
    }
  });
  return runtime;
}

std::atomic<PFN_vkGetDeviceProcAddr> g_get_device_proc_addr{nullptr};

// The FidelityFX Vulkan backend asks for the KHR names of entry points that
// are core since Vulkan 1.1, and crashes on a null pointer when the driver
// only answers to the core names (docs/UPSCALING_BACKLOG.md UP-0.2).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddrCompat(VkDevice device,
                                                                 const char* name) {
  PFN_vkGetDeviceProcAddr get_device_proc_addr =
      g_get_device_proc_addr.load(std::memory_order_relaxed);
  if (!name || !get_device_proc_addr) {
    return nullptr;
  }
  if (PFN_vkVoidFunction proc = get_device_proc_addr(device, name)) {
    return proc;
  }
  static const char* const kAliases[][2] = {
      {"vkGetBufferMemoryRequirements2KHR", "vkGetBufferMemoryRequirements2"},
      {"vkGetImageMemoryRequirements2KHR", "vkGetImageMemoryRequirements2"},
      {"vkBindBufferMemory2KHR", "vkBindBufferMemory2"},
      {"vkBindImageMemory2KHR", "vkBindImageMemory2"},
  };
  for (const auto& alias : kAliases) {
    if (!std::strcmp(name, alias[0])) {
      return get_device_proc_addr(device, alias[1]);
    }
  }
  return nullptr;
}

void LogFfxMessage(uint32_t type, const wchar_t* message) {
  if (!message) {
    return;
  }
  std::string text;
  for (const wchar_t* c = message; *c; ++c) {
    text.push_back(*c < 0x80 ? char(*c) : '?');
  }
  if (type == FFX_API_MESSAGE_TYPE_ERROR) {
    REXLOG_WARN("FidelityFX: {}", text);
  } else {
    REXLOG_DEBUG("FidelityFX: {}", text);
  }
}

class VulkanFfxUpscaler final : public VulkanUpscaler {
 public:
  // major: 2 or 3, the FSR version this provider pins.
  VulkanFfxUpscaler(const VulkanDevice* device, uint32_t major)
      : device_(device), major_(major) {
    info_.id = major == 2 ? "fsr2" : "fsr3";
    info_.name = "AMD FidelityFX Super Resolution " + std::to_string(major);
    info_.kind = UpscalerKind::kTemporal;
    info_.mode_mask = (1u << uint32_t(UpscalerMode::kCount)) - 1;
    info_.uses_depth = true;
    info_.uses_motion_vectors = true;
    info_.uses_jitter = true;
    info_.uses_reactive_mask = true;
    const FfxRuntime& runtime = GetFfxRuntime();
    if (!runtime.loaded()) {
      info_.unavailable_reason = runtime.error;
      return;
    }
    uint64_t count = 8;
    uint64_t ids[8] = {};
    const char* names[8] = {};
    ffxQueryDescGetVersions query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.outputCount = &count;
    query.versionIds = ids;
    query.versionNames = names;
    if (runtime.query(nullptr, &query.header) != FFX_API_RETURN_OK) {
      info_.unavailable_reason = "the runtime lists no upscaler versions";
      return;
    }
    // Names are "3.1.4" and the like, newest first.
    for (uint64_t i = 0; i < std::min<uint64_t>(count, 8); ++i) {
      const char* name = names[i];
      if (!name) {
        continue;
      }
      while (*name && (*name < '0' || *name > '9')) {
        ++name;
      }
      if (uint32_t(*name - '0') == major_ && name[1] == '.') {
        version_id_ = ids[i];
        info_.version = name;
        info_.available = true;
        break;
      }
    }
    if (!info_.available) {
      info_.unavailable_reason = "the runtime has no FSR " + std::to_string(major_) + " version";
    }
  }

  ~VulkanFfxUpscaler() override { Release(); }

  const UpscalerInfo& info() const override { return info_; }

  bool Prepare(uint32_t max_render_width, uint32_t max_render_height, uint32_t max_output_width,
               uint32_t max_output_height, bool hdr) override {
    if (!info_.available) {
      return false;
    }
    if (context_ && max_render_width <= max_render_width_ &&
        max_render_height <= max_render_height_ && max_output_width <= max_output_width_ &&
        max_output_height <= max_output_height_ && hdr == hdr_) {
      return true;
    }
    // A context that failed is not retried every frame for the same sizes.
    const std::array<uint32_t, 5> request = {max_render_width, max_render_height,
                                             max_output_width, max_output_height, uint32_t(hdr)};
    if (request == failed_request_) {
      return false;
    }
    Release();
    const FfxRuntime& runtime = GetFfxRuntime();

    ffxCreateContextDescUpscale create_desc = {};
    create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // SDR guest output is display-referred and gamma encoded, at exposure 1.
    create_desc.flags =
        hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    create_desc.maxRenderSize.width = max_render_width;
    create_desc.maxRenderSize.height = max_render_height;
    create_desc.maxUpscaleSize.width = max_output_width;
    create_desc.maxUpscaleSize.height = max_output_height;
    create_desc.fpMessage = LogFfxMessage;

    ffxCreateBackendVKDesc backend_desc = {};
    backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend_desc.vkDevice = device_->device();
    backend_desc.vkPhysicalDevice = device_->physical_device();
    const VulkanInstance* instance = device_->vulkan_instance();
    g_get_device_proc_addr.store(reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                                     instance->functions().vkGetInstanceProcAddr(
                                         instance->instance(), "vkGetDeviceProcAddr")),
                                 std::memory_order_relaxed);
    backend_desc.vkDeviceProcAddr = GetDeviceProcAddrCompat;
    create_desc.header.pNext = &backend_desc.header;

    ffxOverrideVersion override_desc = {};
    override_desc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_desc.versionId = version_id_;
    backend_desc.header.pNext = &override_desc.header;

    ffxContext context = nullptr;
    const ffxReturnCode_t created = runtime.create_context(&context, &create_desc.header, nullptr);
    if (created != FFX_API_RETURN_OK) {
      REXLOG_WARN("FidelityFX: could not create the FSR {} context ({}x{} to {}x{}): error {}",
                  info_.version, max_render_width, max_render_height, max_output_width,
                  max_output_height, created);
      failed_request_ = request;
      return false;
    }
    failed_request_ = {};
    context_ = context;
    max_render_width_ = max_render_width;
    max_render_height_ = max_render_height;
    max_output_width_ = max_output_width;
    max_output_height_ = max_output_height;
    hdr_ = hdr;
    if (!CreateNeutralInputs()) {
      Release();
      return false;
    }
    REXLOG_INFO("FidelityFX: FSR {} context for {}x{} to {}x{}{}", info_.version, max_render_width,
                max_render_height, max_output_width, max_output_height, hdr ? ", HDR" : "");
    return true;
  }

  bool Dispatch(VkCommandBuffer command_buffer, const VulkanUpscalerInputs& inputs) override {
    if (!context_ || !inputs.color.image || !inputs.output.image) {
      return false;
    }
    const UpscalerFrame& frame = inputs.frame;
    if (neutral_inputs_need_clear_) {
      ClearNeutralInputs(command_buffer);
    }

    VulkanUpscalerImage depth = inputs.depth.image ? inputs.depth : neutral_depth_;
    VulkanUpscalerImage motion_vectors =
        inputs.motion_vectors.image ? inputs.motion_vectors : neutral_motion_vectors_;

    ffxDispatchDescUpscale dispatch_desc = {};
    dispatch_desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch_desc.commandList = command_buffer;
    dispatch_desc.color = Resource(inputs.color, FFX_API_RESOURCE_USAGE_READ_ONLY,
                                   FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    // The depth is an R32_SFLOAT colour image (the camera pass's copy, or
    // the stand-in): FFX would view a DEPTHTARGET as D32_SFLOAT with the
    // depth aspect, which only Metal (MoltenVK) refuses.
    dispatch_desc.depth = Resource(depth, FFX_API_RESOURCE_USAGE_READ_ONLY,
                                   FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch_desc.motionVectors = Resource(motion_vectors, FFX_API_RESOURCE_USAGE_READ_ONLY,
                                           FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    if (inputs.reactive_mask.image) {
      dispatch_desc.reactive = Resource(inputs.reactive_mask, FFX_API_RESOURCE_USAGE_READ_ONLY,
                                        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
    if (inputs.transparency_mask.image) {
      dispatch_desc.transparencyAndComposition =
          Resource(inputs.transparency_mask, FFX_API_RESOURCE_USAGE_READ_ONLY,
                   FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
    dispatch_desc.output = Resource(inputs.output, FFX_API_RESOURCE_USAGE_UAV,
                                    FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch_desc.jitterOffset.x = frame.jitter_x;
    dispatch_desc.jitterOffset.y = frame.jitter_y;
    dispatch_desc.motionVectorScale.x = frame.motion_vector_scale_x;
    dispatch_desc.motionVectorScale.y = frame.motion_vector_scale_y;
    dispatch_desc.renderSize.width = frame.render_width;
    dispatch_desc.renderSize.height = frame.render_height;
    dispatch_desc.upscaleSize.width = frame.output_width;
    dispatch_desc.upscaleSize.height = frame.output_height;
    dispatch_desc.enableSharpening = frame.sharpness > 0.0f;
    dispatch_desc.sharpness = std::clamp(frame.sharpness, 0.0f, 1.0f);
    dispatch_desc.frameTimeDelta = frame.frame_delta_ms;
    dispatch_desc.preExposure = 1.0f;
    dispatch_desc.reset = frame.reset;
    dispatch_desc.cameraNear = frame.camera_near;
    dispatch_desc.cameraFar = frame.camera_far;
    dispatch_desc.cameraFovAngleVertical = frame.camera_fov_y;
    dispatch_desc.viewSpaceToMetersFactor = 1.0f;
    dispatch_desc.flags = hdr_ ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

    ffxContext context = context_;
    return GetFfxRuntime().dispatch(&context, &dispatch_desc.header) == FFX_API_RETURN_OK;
  }

  void Release() override {
    if (context_) {
      ffxContext context = context_;
      GetFfxRuntime().destroy_context(&context, nullptr);
      context_ = nullptr;
    }
    const VulkanDevice::Functions& dfn = device_->functions();
    const VkDevice device = device_->device();
    for (NeutralImage* image : {&neutral_depth_storage_, &neutral_motion_vectors_storage_}) {
      util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, image->view);
      util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image->image);
      util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image->memory);
    }
    neutral_depth_ = {};
    neutral_motion_vectors_ = {};
    max_render_width_ = max_render_height_ = max_output_width_ = max_output_height_ = 0;
  }

 private:
  struct NeutralImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  static FfxApiResource Resource(const VulkanUpscalerImage& image, uint32_t usage,
                                 uint32_t state) {
    VkImageCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.imageType = VK_IMAGE_TYPE_2D;
    create_info.format = image.format;
    create_info.extent = {image.width, image.height, 1};
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.usage = image.usage;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(image.image),
                               ffxApiGetImageResourceDescriptionVK(image.image, create_info, usage),
                               state);
  }

  bool CreateNeutralImage(VkFormat format, NeutralImage& storage, VulkanUpscalerImage& image) {
    VkImageCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.imageType = VK_IMAGE_TYPE_2D;
    create_info.format = format;
    create_info.extent = {max_render_width_, max_render_height_, 1};
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!util::CreateDedicatedAllocationImage(device_, create_info,
                                              util::MemoryPurpose::kDeviceLocal, storage.image,
                                              storage.memory)) {
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = storage.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = util::InitializeSubresourceRange();
    if (device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr,
                                               &storage.view) != VK_SUCCESS) {
      return false;
    }
    image.image = storage.image;
    image.view = storage.view;
    image.format = format;
    image.width = max_render_width_;
    image.height = max_render_height_;
    image.usage = create_info.usage;
    return true;
  }

  // Stand-ins for inputs the frame does not have: depth at the far plane and
  // no motion, so the dispatch runs (and costs) as with real inputs.
  bool CreateNeutralInputs() {
    if (!CreateNeutralImage(VK_FORMAT_R32_SFLOAT, neutral_depth_storage_, neutral_depth_) ||
        !CreateNeutralImage(VK_FORMAT_R16G16_SFLOAT, neutral_motion_vectors_storage_,
                            neutral_motion_vectors_)) {
      return false;
    }
    neutral_inputs_need_clear_ = true;
    return true;
  }

  void ClearNeutralInputs(VkCommandBuffer command_buffer) {
    const VulkanDevice::Functions& dfn = device_->functions();
    VkImageMemoryBarrier barriers[2] = {};
    VkImage images[2] = {neutral_depth_.image, neutral_motion_vectors_.image};
    for (uint32_t i = 0; i < 2; ++i) {
      barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].image = images[i];
      barriers[i].subresourceRange = util::InitializeSubresourceRange();
    }
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                             barriers);
    const VkImageSubresourceRange range = util::InitializeSubresourceRange();
    VkClearColorValue far_depth = {};
    far_depth.float32[0] = 1.0f;
    dfn.vkCmdClearColorImage(command_buffer, neutral_depth_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_depth, 1, &range);
    VkClearColorValue no_motion = {};
    dfn.vkCmdClearColorImage(command_buffer, neutral_motion_vectors_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &no_motion, 1, &range);
    for (VkImageMemoryBarrier& barrier : barriers) {
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                             barriers);
    neutral_inputs_need_clear_ = false;
  }

  const VulkanDevice* device_;
  uint32_t major_;
  UpscalerInfo info_;
  uint64_t version_id_ = 0;
  void* context_ = nullptr;
  uint32_t max_render_width_ = 0;
  uint32_t max_render_height_ = 0;
  uint32_t max_output_width_ = 0;
  uint32_t max_output_height_ = 0;
  bool hdr_ = false;
  std::array<uint32_t, 5> failed_request_ = {};
  NeutralImage neutral_depth_storage_;
  NeutralImage neutral_motion_vectors_storage_;
  VulkanUpscalerImage neutral_depth_;
  VulkanUpscalerImage neutral_motion_vectors_;
  bool neutral_inputs_need_clear_ = false;
};

// A FidelityFX API resource for a Vulkan image.
FfxApiResource FfxImageResource(const VulkanUpscalerImage& image, uint32_t usage, uint32_t state) {
  VkImageCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  create_info.imageType = VK_IMAGE_TYPE_2D;
  create_info.format = image.format;
  create_info.extent = {image.width, image.height, 1};
  create_info.mipLevels = 1;
  create_info.arrayLayers = 1;
  create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  create_info.usage = image.usage;
  return ffxApiGetResourceVK(reinterpret_cast<void*>(image.image),
                             ffxApiGetImageResourceDescriptionVK(image.image, create_info, usage),
                             state);
}

// FSR 3.1 frame generation: the API's frame generation provider, dispatched
// by the presenter itself (configure, prepare with depth and motion, then
// interpolate into an image of ours). AMD's swapchain replacement is not
// used, so pacing and presentation stay the presenter's.
class VulkanFfxFrameGenerator final : public VulkanFrameGenerator {
 public:
  explicit VulkanFfxFrameGenerator(const VulkanDevice* device) : device_(device) {
    info_.id = "fsr3";
    info_.name = "AMD FidelityFX Super Resolution 3 frame generation";
    info_.uses_depth = true;
    info_.uses_motion_vectors = true;
    const FfxRuntime& runtime = GetFfxRuntime();
    if (!runtime.loaded()) {
      info_.unavailable_reason = runtime.error;
      return;
    }
    uint64_t count = 8;
    uint64_t ids[8] = {};
    const char* names[8] = {};
    ffxQueryDescGetVersions query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    query.outputCount = &count;
    query.versionIds = ids;
    query.versionNames = names;
    if (runtime.query(nullptr, &query.header) != FFX_API_RETURN_OK || !count || !names[0]) {
      info_.unavailable_reason = "the FidelityFX runtime has no frame generation";
      return;
    }
    info_.version = names[0];
    version_id_ = ids[0];
    info_.available = true;
  }
  ~VulkanFfxFrameGenerator() override { Release(); }

  const FrameGeneratorInfo& info() const override { return info_; }

  bool Prepare(uint32_t display_width, uint32_t display_height, uint32_t max_render_width,
               uint32_t max_render_height) override {
    if (!info_.available) {
      return false;
    }
    if (context_ && display_width == display_width_ && display_height == display_height_ &&
        max_render_width <= max_render_width_ && max_render_height <= max_render_height_) {
      return true;
    }
    const std::array<uint32_t, 4> request = {display_width, display_height, max_render_width,
                                             max_render_height};
    if (request == failed_request_) {
      return false;
    }
    Release();
    ffxCreateContextDescFrameGeneration create_desc = {};
    create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create_desc.flags = 0;
    create_desc.displaySize = {display_width, display_height};
    create_desc.maxRenderSize = {max_render_width, max_render_height};
    create_desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxCreateBackendVKDesc backend_desc = {};
    backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend_desc.vkDevice = device_->device();
    backend_desc.vkPhysicalDevice = device_->physical_device();
    const VulkanInstance* instance = device_->vulkan_instance();
    g_get_device_proc_addr.store(reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                                     instance->functions().vkGetInstanceProcAddr(
                                         instance->instance(), "vkGetDeviceProcAddr")),
                                 std::memory_order_relaxed);
    backend_desc.vkDeviceProcAddr = GetDeviceProcAddrCompat;
    create_desc.header.pNext = &backend_desc.header;
    ffxOverrideVersion override_desc = {};
    override_desc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_desc.versionId = version_id_;
    backend_desc.header.pNext = &override_desc.header;
    ffxContext context = nullptr;
    const ffxReturnCode_t created =
        GetFfxRuntime().create_context(&context, &create_desc.header, nullptr);
    if (created != FFX_API_RETURN_OK) {
      REXLOG_WARN(
          "FidelityFX: could not create the frame generation context ({}x{}, render up to "
          "{}x{}): error {}",
          display_width, display_height, max_render_width, max_render_height, created);
      failed_request_ = request;
      return false;
    }
    failed_request_ = {};
    context_ = context;
    display_width_ = display_width;
    display_height_ = display_height;
    max_render_width_ = max_render_width;
    max_render_height_ = max_render_height;
    REXLOG_INFO("FidelityFX: frame generation {} for {}x{}, render up to {}x{}", info_.version,
                display_width, display_height, max_render_width, max_render_height);
    return true;
  }

  bool Dispatch(VkCommandBuffer command_buffer,
                const VulkanFrameGeneratorInputs& inputs) override {
    if (!context_ || !inputs.present_color.image || !inputs.output.image ||
        !inputs.depth.image || !inputs.motion_vectors.image) {
      return false;
    }
    const FfxRuntime& runtime = GetFfxRuntime();
    const UpscalerFrame& frame = inputs.frame;
    ffxContext context = context_;
    // No swapchain to notify: this only runs the interpolation.
    ffxConfigureDescFrameGeneration configure = {};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.frameGenerationEnabled = true;
    configure.allowAsyncWorkloads = false;
    configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    if (REXCVAR_GET(present_ffx_frame_generation_debug)) {
      configure.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
    }
    configure.generationRect = {inputs.generation_x, inputs.generation_y,
                                int32_t(inputs.generation_width),
                                int32_t(inputs.generation_height)};
    configure.frameID = inputs.frame_id;
    if (runtime.configure(&context, &configure.header) != FFX_API_RETURN_OK) {
      return false;
    }
    ffxDispatchDescFrameGenerationPrepare prepare = {};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.frameID = inputs.frame_id;
    prepare.commandList = command_buffer;
    prepare.renderSize = {frame.render_width, frame.render_height};
    prepare.jitterOffset = {frame.jitter_x, frame.jitter_y};
    prepare.motionVectorScale = {frame.motion_vector_scale_x, frame.motion_vector_scale_y};
    prepare.frameTimeDelta = frame.frame_delta_ms;
    prepare.cameraNear = frame.camera_near;
    prepare.cameraFar = frame.camera_far;
    prepare.cameraFovAngleVertical = frame.camera_fov_y;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = FfxImageResource(inputs.depth, FFX_API_RESOURCE_USAGE_READ_ONLY,
                                     FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prepare.motionVectors =
        FfxImageResource(inputs.motion_vectors, FFX_API_RESOURCE_USAGE_READ_ONLY,
                         FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    if (runtime.dispatch(&context, &prepare.header) != FFX_API_RETURN_OK) {
      return false;
    }
    ffxDispatchDescFrameGeneration dispatch = {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    dispatch.commandList = command_buffer;
    dispatch.presentColor =
        FfxImageResource(inputs.present_color, FFX_API_RESOURCE_USAGE_READ_ONLY,
                         FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.outputs[0] = FfxImageResource(inputs.output, FFX_API_RESOURCE_USAGE_UAV,
                                           FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.numGeneratedFrames = 1;
    dispatch.reset = frame.reset;
    dispatch.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    dispatch.minMaxLuminance[0] = 0.0f;
    dispatch.minMaxLuminance[1] = 1.0f;
    dispatch.generationRect = configure.generationRect;
    dispatch.frameID = inputs.frame_id;
    return runtime.dispatch(&context, &dispatch.header) == FFX_API_RETURN_OK;
  }

  void Release() override {
    if (context_) {
      ffxContext context = context_;
      GetFfxRuntime().destroy_context(&context, nullptr);
      context_ = nullptr;
    }
    display_width_ = display_height_ = max_render_width_ = max_render_height_ = 0;
  }

 private:
  const VulkanDevice* device_;
  FrameGeneratorInfo info_;
  uint64_t version_id_ = 0;
  void* context_ = nullptr;
  uint32_t display_width_ = 0;
  uint32_t display_height_ = 0;
  uint32_t max_render_width_ = 0;
  uint32_t max_render_height_ = 0;
  std::array<uint32_t, 4> failed_request_ = {};
};

}  // namespace
#endif  // defined(REX_HAS_FFX_API) && REX_HAS_FFX_API

std::vector<std::unique_ptr<VulkanUpscaler>> CreateVulkanUpscalers(
    [[maybe_unused]] const VulkanDevice* device) {
  std::vector<std::unique_ptr<VulkanUpscaler>> upscalers;
  if (std::unique_ptr<VulkanUpscaler> dlss = CreateVulkanDlssUpscaler(device)) {
    upscalers.push_back(std::move(dlss));
  }
#if defined(REX_HAS_FFX_API) && REX_HAS_FFX_API
  upscalers.push_back(std::make_unique<VulkanFfxUpscaler>(device, 3));
  upscalers.push_back(std::make_unique<VulkanFfxUpscaler>(device, 2));
#endif
  return upscalers;
}

std::unique_ptr<VulkanFrameGenerator> CreateVulkanFfxFrameGenerator(
    [[maybe_unused]] const VulkanDevice* device) {
#if defined(REX_HAS_FFX_API) && REX_HAS_FFX_API
  return std::make_unique<VulkanFfxFrameGenerator>(device);
#else
  return nullptr;
#endif
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
