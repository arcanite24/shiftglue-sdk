/**
 * @file        ui/vulkan/vulkan_openxr.cpp
 * @brief       The title's output in a VR headset through OpenXR.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/vulkan/openxr.h>

#include <atomic>
#include <cmath>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(xr_enabled, false, "Display",
                    "Show the game in a VR headset through the PC's OpenXR runtime (Virtual "
                    "Desktop, SteamVR, Meta Link, ...). Read at start; Vulkan only");
REXCVAR_DEFINE_BOOL(xr_stereo, true, "Display",
                    "In VR, render the cockpit in 3D, the eyes taking turns frame by frame, "
                    "when the game supports it; otherwise, or off, a large screen")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(xr_screen_distance, 2.5, "Display",
                      "Distance in metres from the headset's start position to the VR screen")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(xr_screen_width, 3.2, "Display", "Width of the VR screen in metres")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(xr_simulate, false, "Display",
                    "Testing without a headset: request alternating eyes at a fixed head pose "
                    "and log how presented frames decode, without OpenXR")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(xr_simulate_ipd, 0.064, "Display",
                      "Eye distance in metres for xr_simulate")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex {
namespace ui {
namespace vulkan {
namespace openxr {

namespace {

constexpr double kFovStep = 0.002;
constexpr double kTagStep = kFovStep / 32.0;

// What NextEye handed out, by tag, for presented frames to find again.
struct Requested {
  bool valid = false;
  EyeRequest request;
};

struct Bookkeeping {
  std::mutex mutex;
  uint64_t sequence = 0;
  Requested ring[kTagCount];
  // Log: frames decoded per eye, untagged, lens missing.
  uint64_t decoded[2] = {}, untagged = 0, no_lens = 0, last_logged = 0;
  // Eye check: between consecutive decoded frames, does the camera move
  // sideways the way their requests do?
  bool have_previous = false;
  Lens previous_lens;
  EyeRequest previous_request;
  uint64_t eye_match = 0, eye_mismatch = 0, detailed = 0;
};

Bookkeeping& B() {
  static Bookkeeping bookkeeping;
  return bookkeeping;
}

// The request a presented frame's lens tags, if any.
bool FindRequest(const Lens& lens, EyeRequest& out) {
  Bookkeeping& b = B();
  std::lock_guard<std::mutex> lock(b.mutex);
  if (!lens.valid) {
    ++b.no_lens;
    return false;
  }
  const uint32_t tag = FovTag(lens.fov_y);
  if (tag >= kTagCount || !b.ring[tag].valid) {
    ++b.untagged;
    return false;
  }
  out = b.ring[tag].request;
  ++b.decoded[out.eye & 1];
  if (b.have_previous && lens.has_position && b.previous_lens.has_position) {
    double moved = 0.0;
    for (int i = 0; i < 3; ++i) {
      moved += double(lens.position[i] - b.previous_lens.position[i]) * lens.right[i];
    }
    const double expected = double(out.position[0]) - b.previous_request.position[0];
    if (std::abs(expected) > 0.01) {
      ++((moved > 0.0) == (expected > 0.0) ? b.eye_match : b.eye_mismatch);
    }
    if (b.detailed < 12) {
      ++b.detailed;
      REXLOG_INFO("OpenXR frame: tag {} eye {} fov_y {:.6f}, moved {:+.4f} expected {:+.4f}",
                  out.tag, out.eye, lens.fov_y, moved, expected);
    }
  }
  b.have_previous = true;
  b.previous_lens = lens;
  b.previous_request = out;
  return true;
}

void LogDecoding(const char* where) {
  Bookkeeping& b = B();
  std::lock_guard<std::mutex> lock(b.mutex);
  const uint64_t total = b.decoded[0] + b.decoded[1] + b.untagged + b.no_lens;
  if (total < b.last_logged + 300) return;
  b.last_logged = total;
  REXLOG_INFO(
      "OpenXR {}: {} requested, presented left {} right {}, untagged {}, no lens {}; "
      "eye check {} right, {} wrong",
      where, b.sequence, b.decoded[0], b.decoded[1], b.untagged, b.no_lens, b.eye_match,
      b.eye_mismatch);
}

// Stores and returns a new request for `eye` at this pose.
EyeRequest Record(int eye, const float position[3], const float orientation[4]) {
  Bookkeeping& b = B();
  std::lock_guard<std::mutex> lock(b.mutex);
  EyeRequest request;
  request.eye = eye;
  request.tag = uint32_t(b.sequence % kTagCount);
  for (int i = 0; i < 3; ++i) request.position[i] = position[i];
  for (int i = 0; i < 4; ++i) request.orientation[i] = orientation[i];
  b.ring[request.tag] = {true, request};
  ++b.sequence;
  return request;
}

}  // namespace

double TagFov(double fov_y, uint32_t tag) {
  if (!(fov_y > 0.0)) return fov_y;
  return std::floor(fov_y / kFovStep) * kFovStep + (double(tag % kTagCount) + 0.5) * kTagStep;
}

uint32_t FovTag(double fov_y) {
  if (!(fov_y > 0.0)) return UINT32_MAX;
  const double residual = fov_y - std::floor(fov_y / kFovStep) * kFovStep;
  const double slot = residual / kTagStep - 0.5;
  const double nearest = std::round(slot);
  // An untagged field of view lands anywhere in the step; a tag lands within
  // a tenth of a slot of its centre, in the lower half of the step.
  if (nearest < 0.0 || nearest >= double(kTagCount) || std::abs(slot - nearest) > 0.1) {
    return UINT32_MAX;
  }
  return uint32_t(nearest);
}

}  // namespace openxr
}  // namespace vulkan
}  // namespace ui
}  // namespace rex

#if REX_HAS_OPENXR

#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <string>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

namespace rex {
namespace ui {
namespace vulkan {
namespace openxr {

namespace {

// One swapchain the guest output is copied into: the screen, or an eye.
struct Target {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  std::vector<XrSwapchainImageVulkanKHR> images;
  bool acquired = false;
  uint32_t index = 0;
  // An eye's last released frame: what it was rendered with.
  bool has_frame = false;
  XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  XrFovf fov{};
};

enum TargetIndex { kLeft, kRight, kScreen, kTargetCount };

struct State {
  std::mutex mutex;
  bool instance_tried = false;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions = nullptr;
  PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions = nullptr;
  PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device = nullptr;
  PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements = nullptr;
  // Extension names handed to Vulkan, kept for the life of the process.
  std::deque<std::string> names;

  const VulkanDevice* device = nullptr;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  XrSpace view = XR_NULL_HANDLE;
  std::atomic<bool> running{false};
  // The display time NextEye locates the eyes at: the latest frame's, one
  // period ahead (the game's frame shows about then).
  std::atomic<int64_t> next_display_time{0};
  std::atomic<int64_t> display_period{0};

  VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
  VkFormat staging_format = VK_FORMAT_UNDEFINED;  // the UNORM twin of swapchain_format
  Target targets[kTargetCount];
  // The guest output is 10:10:10:2 and gamma-encoded; the headset wants 8-bit
  // sRGB. A blit into an 8-bit UNORM image keeps the encoded values, and a
  // raw copy of those bits into the sRGB image then shows them as sRGB.
  VkImage staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  uint32_t staging_width = 0, staging_height = 0;
  // Images replaced on a resize, freed at exit (the GPU may still read them).
  std::vector<std::pair<VkImage, VkDeviceMemory>> retired;

  bool frame_begun = false;
  // What this frame shows: the screen, or the eyes.
  bool frame_stereo = false;
  bool frame_has_image = false;
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};
  std::chrono::steady_clock::time_point last_pose_log{};
  uint64_t frames = 0;
};

State& S() {
  static State state;
  return state;
}

bool Check(XrResult result, const char* what) {
  if (XR_SUCCEEDED(result)) return true;
  char text[XR_MAX_RESULT_STRING_SIZE] = {};
  if (S().instance != XR_NULL_HANDLE) xrResultToString(S().instance, result, text);
  REXLOG_WARN("OpenXR: {} failed: {} ({})", what, text[0] ? text : "error", int(result));
  return false;
}

template <typename T>
bool Proc(XrInstance instance, const char* name, T& out) {
  return Check(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out)),
               name);
}

// Splits the runtime's space-separated list into names that stay valid.
std::vector<const char*> Names(const std::string& list) {
  std::vector<const char*> out;
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find(' ', start);
    if (end == std::string::npos) end = list.size();
    if (end > start) out.push_back(S().names.emplace_back(list.substr(start, end - start)).c_str());
    start = end + 1;
  }
  return out;
}

bool EnsureInstance() {
  State& s = S();
  if (s.instance_tried) return s.instance != XR_NULL_HANDLE;
  s.instance_tried = true;
  if (!REXCVAR_GET(xr_enabled)) return false;

  uint32_t count = 0;
  if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
             "listing the runtime's extensions (is an OpenXR runtime installed?)")) {
    return false;
  }
  std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
  bool has_vulkan = false;
  for (const auto& extension : extensions) {
    has_vulkan |= std::strcmp(extension.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
  }
  if (!has_vulkan) {
    REXLOG_WARN("OpenXR: the runtime has no {}; VR is off", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    return false;
  }

  XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(create_info.applicationInfo.applicationName, "ReXGlue title",
               XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(create_info.applicationInfo.engineName, "ReXGlue", XR_MAX_ENGINE_NAME_SIZE - 1);
  create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  const char* enabled[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
  create_info.enabledExtensionCount = 1;
  create_info.enabledExtensionNames = enabled;
  if (!Check(xrCreateInstance(&create_info, &s.instance), "xrCreateInstance")) return false;

  XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(s.instance, &properties))) {
    REXLOG_INFO("OpenXR: runtime {} {}.{}.{}", properties.runtimeName,
                XR_VERSION_MAJOR(properties.runtimeVersion),
                XR_VERSION_MINOR(properties.runtimeVersion),
                XR_VERSION_PATCH(properties.runtimeVersion));
  }
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (!Check(xrGetSystem(s.instance, &system_info, &s.system),
             "finding a headset (is it connected and awake?)") ||
      !Proc(s.instance, "xrGetVulkanInstanceExtensionsKHR", s.get_instance_extensions) ||
      !Proc(s.instance, "xrGetVulkanDeviceExtensionsKHR", s.get_device_extensions) ||
      !Proc(s.instance, "xrGetVulkanGraphicsDeviceKHR", s.get_graphics_device) ||
      !Proc(s.instance, "xrGetVulkanGraphicsRequirementsKHR", s.get_graphics_requirements)) {
    xrDestroyInstance(s.instance);
    s.instance = XR_NULL_HANDLE;
    return false;
  }
  XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(xrGetSystemProperties(s.instance, s.system, &system_properties))) {
    REXLOG_INFO("OpenXR: headset {}", system_properties.systemName);
  }
  return true;
}

template <typename Get>
std::string ExtensionList(Get get) {
  State& s = S();
  uint32_t size = 0;
  if (!Check(get(s.instance, s.system, 0, &size, nullptr), "listing the Vulkan extensions")) {
    return {};
  }
  std::string list(size, '\0');
  get(s.instance, s.system, size, &size, list.data());
  while (!list.empty() && list.back() == '\0') list.pop_back();
  return list;
}

void DestroyTarget(Target& target) {
  if (target.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(target.swapchain);
  target = {};
}

void DestroySession() {
  State& s = S();
  // NextEye (the game thread) stops using the session first.
  s.running = false;
  s.next_display_time = 0;
  for (Target& target : s.targets) DestroyTarget(target);
  if (s.view != XR_NULL_HANDLE) xrDestroySpace(s.view);
  if (s.local != XR_NULL_HANDLE) xrDestroySpace(s.local);
  if (s.session != XR_NULL_HANDLE) xrDestroySession(s.session);
  s.view = s.local = XR_NULL_HANDLE;
  s.session = XR_NULL_HANDLE;
  s.running = false;
}

void PollEvents() {
  State& s = S();
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (s.instance != XR_NULL_HANDLE && xrPollEvent(s.instance, &event) == XR_SUCCESS) {
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
      REXLOG_INFO("OpenXR: session state {}", int(changed.state));
      if (changed.state == XR_SESSION_STATE_READY && !s.running) {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        s.running = Check(xrBeginSession(s.session, &begin), "xrBeginSession");
      } else if (changed.state == XR_SESSION_STATE_STOPPING && s.running) {
        s.running = false;
        xrEndSession(s.session);
      } else if (changed.state == XR_SESSION_STATE_EXITING ||
                 changed.state == XR_SESSION_STATE_LOSS_PENDING) {
        REXLOG_INFO("OpenXR: the runtime ended the session; VR is off until restart");
        DestroySession();
      }
    } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      DestroySession();
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

bool ChooseFormat() {
  State& s = S();
  if (s.swapchain_format != VK_FORMAT_UNDEFINED) return true;
  uint32_t count = 0;
  xrEnumerateSwapchainFormats(s.session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  xrEnumerateSwapchainFormats(s.session, count, &count, formats.data());
  for (const VkFormat wanted : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
    for (const int64_t format : formats) {
      if (format == wanted && s.swapchain_format == VK_FORMAT_UNDEFINED) {
        s.swapchain_format = wanted;
      }
    }
  }
  if (s.swapchain_format == VK_FORMAT_UNDEFINED) {
    REXLOG_WARN("OpenXR: the runtime offers no 8-bit sRGB swapchain format; VR is off");
    return false;
  }
  s.staging_format = s.swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
                                                                   : VK_FORMAT_B8G8R8A8_UNORM;
  return true;
}

bool EnsureStaging(uint32_t width, uint32_t height) {
  State& s = S();
  if (s.staging != VK_NULL_HANDLE && s.staging_width == width && s.staging_height == height) {
    return true;
  }
  if (s.staging != VK_NULL_HANDLE) s.retired.emplace_back(s.staging, s.staging_memory);
  s.staging = VK_NULL_HANDLE;
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = s.staging_format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!util::CreateDedicatedAllocationImage(s.device, image_info, util::MemoryPurpose::kDeviceLocal,
                                            s.staging, s.staging_memory)) {
    REXLOG_WARN("OpenXR: could not create the {}x{} staging image", width, height);
    s.staging = VK_NULL_HANDLE;
    return false;
  }
  s.staging_width = width;
  s.staging_height = height;
  return true;
}

bool EnsureTarget(Target& target, uint32_t width, uint32_t height, const char* name) {
  State& s = S();
  if (target.swapchain != XR_NULL_HANDLE && target.width == width && target.height == height) {
    return true;
  }
  DestroyTarget(target);
  XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  create_info.usageFlags =
      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  create_info.format = s.swapchain_format;
  create_info.sampleCount = 1;
  create_info.width = width;
  create_info.height = height;
  create_info.faceCount = 1;
  create_info.arraySize = 1;
  create_info.mipCount = 1;
  if (!Check(xrCreateSwapchain(s.session, &create_info, &target.swapchain), "xrCreateSwapchain")) {
    target.swapchain = XR_NULL_HANDLE;
    return false;
  }
  uint32_t count = 0;
  xrEnumerateSwapchainImages(target.swapchain, 0, &count, nullptr);
  target.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  xrEnumerateSwapchainImages(target.swapchain, count, &count,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(target.images.data()));
  target.width = width;
  target.height = height;
  REXLOG_INFO("OpenXR: {} {}x{}, {} swapchain images", name, width, height, count);
  return true;
}

bool Acquire(Target& target) {
  State& s = S();
  {
    // The runtime may use the game's queue here (XR_KHR_vulkan_enable).
    const auto queue = s.device->AcquireQueue(s.device->queue_family_graphics_compute(), 0);
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(target.swapchain, &acquire_info, &target.index),
               "xrAcquireSwapchainImage")) {
      return false;
    }
  }
  target.acquired = true;
  XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  image_wait.timeout = 100'000'000;  // 100 ms
  return Check(xrWaitSwapchainImage(target.swapchain, &image_wait), "xrWaitSwapchainImage");
}

void RecordBlitAndCopy(VkCommandBuffer command_buffer, VkImage source, VkImage target,
                       uint32_t width, uint32_t height, VkImageLayout source_layout,
                       VkAccessFlags source_access, VkPipelineStageFlags source_stage) {
  State& s = S();
  const VulkanDevice::Functions& dfn = s.device->functions();
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  auto barrier = [&](VkImage image, VkAccessFlags from_access, VkAccessFlags to_access,
                     VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = from_access;
    b.dstAccessMask = to_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    return b;
  };

  // The guest output to a transfer source; the staging image (last read by
  // the previous frame's copy) and the headset's image to transfer targets.
  std::array<VkImageMemoryBarrier, 3> before = {
      barrier(source, source_access, VK_ACCESS_TRANSFER_READ_BIT, source_layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
      barrier(s.staging, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      barrier(target, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
  dfn.vkCmdPipelineBarrier(command_buffer,
                           source_stage | VK_PIPELINE_STAGE_TRANSFER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                           uint32_t(before.size()), before.data());

  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
  blit.dstSubresource = blit.srcSubresource;
  blit.dstOffsets[1] = blit.srcOffsets[1];
  dfn.vkCmdBlitImage(command_buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.staging,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

  std::array<VkImageMemoryBarrier, 2> middle = {
      barrier(s.staging, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
      barrier(source, VK_ACCESS_TRANSFER_READ_BIT, source_access,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_layout)};
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT | source_stage, 0, 0, nullptr, 0,
                           nullptr, uint32_t(middle.size()), middle.data());

  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.dstSubresource = copy.srcSubresource;
  copy.extent = {width, height, 1};
  dfn.vkCmdCopyImage(command_buffer, s.staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  // The runtime takes the image back as a color attachment.
  VkImageMemoryBarrier after =
      barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &after);
}

void LogPose() {
  State& s = S();
  const auto now = std::chrono::steady_clock::now();
  if (now - s.last_pose_log < std::chrono::seconds(5)) return;
  s.last_pose_log = now;
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(s.view, s.local, s.frame_state.predictedDisplayTime, &location))) {
    return;
  }
  const XrPosef& p = location.pose;
  REXLOG_INFO(
      "OpenXR: head at ({:.3f}, {:.3f}, {:.3f}) m, orientation ({:.3f}, {:.3f}, {:.3f}, {:.3f}), "
      "{} frames",
      p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y,
      p.orientation.z, p.orientation.w, s.frames);
}

}  // namespace

std::vector<const char*> InstanceExtensions() {
  std::lock_guard<std::mutex> lock(S().mutex);
  if (!EnsureInstance()) return {};
  std::vector<const char*> names = Names(ExtensionList(S().get_instance_extensions));
  for (const char* name : names) REXLOG_INFO("OpenXR: the runtime wants instance extension {}", name);
  return names;
}

std::vector<const char*> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical_device) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  if (s.instance == XR_NULL_HANDLE) return {};
  // Only for the GPU the headset is on.
  VkPhysicalDevice wanted = VK_NULL_HANDLE;
  if (!Check(s.get_graphics_device(s.instance, s.system, instance, &wanted), "finding its GPU") ||
      wanted != physical_device) {
    return {};
  }
  std::vector<const char*> names = Names(ExtensionList(s.get_device_extensions));
  for (const char* name : names) REXLOG_INFO("OpenXR: the runtime wants device extension {}", name);
  return names;
}

void OnDeviceCreated(const VulkanInstance* instance, const VulkanDevice* device) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  if (s.instance == XR_NULL_HANDLE || s.session != XR_NULL_HANDLE || !device) return;
  VkPhysicalDevice wanted = VK_NULL_HANDLE;
  s.get_graphics_device(s.instance, s.system, instance->instance(), &wanted);
  if (wanted != device->physical_device()) {
    REXLOG_WARN("OpenXR: the headset is on another GPU than the game's; VR is off "
                "(choose that GPU with vulkan_device)");
    return;
  }
  // Blitting the 10:10:10:2 guest output needs blit support for it.
  VkFormatProperties format_properties{};
  instance->functions().vkGetPhysicalDeviceFormatProperties(
      device->physical_device(), VK_FORMAT_A2B10G10R10_UNORM_PACK32, &format_properties);
  if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
    REXLOG_WARN("OpenXR: this GPU cannot blit the guest output's format; VR is off");
    return;
  }
  XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  if (!Check(s.get_graphics_requirements(s.instance, s.system, &requirements),
             "xrGetVulkanGraphicsRequirementsKHR")) {
    return;
  }
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  binding.instance = instance->instance();
  binding.physicalDevice = device->physical_device();
  binding.device = device->device();
  binding.queueFamilyIndex = device->queue_family_graphics_compute();
  binding.queueIndex = 0;
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &binding;
  session_info.systemId = s.system;
  if (!Check(xrCreateSession(s.instance, &session_info, &s.session), "xrCreateSession")) {
    s.session = XR_NULL_HANDLE;
    return;
  }
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.poseInReferenceSpace.orientation.w = 1.0f;
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  xrCreateReferenceSpace(s.session, &space_info, &s.local);
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  xrCreateReferenceSpace(s.session, &space_info, &s.view);
  s.device = device;
  REXLOG_INFO("OpenXR: session created; put the headset on");
}

bool StereoWanted() {
  if (REXCVAR_GET(xr_simulate)) return true;
  return REXCVAR_GET(xr_stereo) && S().running.load(std::memory_order_relaxed);
}

bool NextEye(EyeRequest& out) {
  const uint64_t sequence = [] {
    std::lock_guard<std::mutex> lock(B().mutex);
    return B().sequence;
  }();
  const int eye = int(sequence & 1);
  if (REXCVAR_GET(xr_simulate)) {
    const float half = float(REXCVAR_GET(xr_simulate_ipd)) * 0.5f;
    const float position[3] = {eye ? half : -half, 0.0f, 0.0f};
    const float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    out = Record(eye, position, orientation);
    return true;
  }
  State& s = S();
  if (!REXCVAR_GET(xr_stereo) || !s.running.load(std::memory_order_relaxed)) return false;
  // xrLocateViews may run beside the painting thread's frame calls.
  const int64_t time = s.next_display_time.load(std::memory_order_relaxed);
  if (time == 0) return false;
  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = time;
  locate_info.space = s.local;
  XrViewState view_state{XR_TYPE_VIEW_STATE};
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  uint32_t count = 0;
  if (XR_FAILED(xrLocateViews(s.session, &locate_info, &view_state, 2, &count, views)) ||
      count != 2 || !(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
      !(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
    return false;
  }
  const XrPosef& pose = views[eye].pose;
  const float position[3] = {pose.position.x, pose.position.y, pose.position.z};
  const float orientation[4] = {pose.orientation.x, pose.orientation.y, pose.orientation.z,
                                pose.orientation.w};
  out = Record(eye, position, orientation);
  return true;
}

bool BeginFrame(uint32_t width, uint32_t height) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  s.frame_begun = false;
  s.frame_has_image = false;
  if (s.session == XR_NULL_HANDLE) return false;
  PollEvents();
  if (!s.running || width == 0 || height == 0) return false;

  s.frame_state = {XR_TYPE_FRAME_STATE};
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  if (!Check(xrWaitFrame(s.session, &wait_info, &s.frame_state), "xrWaitFrame")) return false;
  s.display_period = s.frame_state.predictedDisplayPeriod;
  s.next_display_time =
      s.frame_state.predictedDisplayTime + s.frame_state.predictedDisplayPeriod;
  {
    const auto queue = s.device->AcquireQueue(s.device->queue_family_graphics_compute(), 0);
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(s.session, &begin_info), "xrBeginFrame")) return false;
  }
  s.frame_begun = true;
  if (!s.frame_state.shouldRender || !ChooseFormat() || !EnsureStaging(width, height)) {
    return false;
  }
  return true;
}

void RecordCopy(VkCommandBuffer command_buffer, VkImage source, uint32_t width, uint32_t height,
                VkImageLayout source_layout, VkAccessFlags source_access,
                VkPipelineStageFlags source_stage, const Lens& lens) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  if (!s.frame_begun || width != s.staging_width || height != s.staging_height) return;
  EyeRequest request;
  const bool stereo = REXCVAR_GET(xr_stereo) && FindRequest(lens, request);
  Target& target = s.targets[stereo ? (request.eye ? kRight : kLeft) : kScreen];
  static const char* const kNames[] = {"left eye", "right eye", "screen"};
  if (!EnsureTarget(target, width, height, kNames[stereo ? request.eye : kScreen]) ||
      !Acquire(target)) {
    return;
  }
  RecordBlitAndCopy(command_buffer, source, target.images[target.index].image, width, height,
                    source_layout, source_access, source_stage);
  s.frame_has_image = true;
  s.frame_stereo = stereo;
  if (stereo) {
    target.has_frame = true;
    target.pose.position = {request.position[0], request.position[1], request.position[2]};
    target.pose.orientation = {request.orientation[0], request.orientation[1],
                               request.orientation[2], request.orientation[3]};
    const float half_x = lens.fov_x * 0.5f, half_y = lens.fov_y * 0.5f;
    target.fov = {-half_x, half_x, half_y, -half_y};
  }
  LogDecoding("stereo");
}

void NoteLens(const Lens& lens) {
  EyeRequest request;
  FindRequest(lens, request);
  LogDecoding("simulated");
}

void EndFrame() {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  if (!s.frame_begun) return;
  s.frame_begun = false;
  const auto queue = s.device->AcquireQueue(s.device->queue_family_graphics_compute(), 0);
  for (Target& target : s.targets) {
    if (!target.acquired) continue;
    target.acquired = false;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    Check(xrReleaseSwapchainImage(target.swapchain, &release_info), "xrReleaseSwapchainImage");
  }
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                               {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  const XrCompositionLayerBaseHeader* layers[1];
  uint32_t layer_count = 0;
  const Target& left = s.targets[kLeft];
  const Target& right = s.targets[kRight];
  if (s.frame_has_image && s.frame_stereo && left.has_frame && right.has_frame) {
    for (int eye = 0; eye < 2; ++eye) {
      const Target& t = s.targets[eye];
      views[eye].pose = t.pose;
      views[eye].fov = t.fov;
      views[eye].subImage.swapchain = t.swapchain;
      views[eye].subImage.imageRect = {{0, 0}, {int32_t(t.width), int32_t(t.height)}};
    }
    projection.space = s.local;
    projection.viewCount = 2;
    projection.views = views;
    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    ++s.frames;
  } else if (s.frame_has_image && !s.frame_stereo && s.targets[kScreen].swapchain) {
    const Target& screen = s.targets[kScreen];
    const float width_m = float(REXCVAR_GET(xr_screen_width));
    quad.space = s.local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = screen.swapchain;
    quad.subImage.imageRect = {{0, 0}, {int32_t(screen.width), int32_t(screen.height)}};
    quad.pose.orientation.w = 1.0f;
    quad.pose.position.z = -float(REXCVAR_GET(xr_screen_distance));
    quad.size = {width_m, width_m * float(screen.height) / float(screen.width)};
    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    ++s.frames;
  }
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = s.frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end_info.layerCount = layer_count;
  end_info.layers = layers;
  Check(xrEndFrame(s.session, &end_info), "xrEndFrame");
  LogPose();
}

}  // namespace openxr
}  // namespace vulkan
}  // namespace ui
}  // namespace rex

#else  // !REX_HAS_OPENXR

namespace rex {
namespace ui {
namespace vulkan {
namespace openxr {

std::vector<const char*> InstanceExtensions() { return {}; }
std::vector<const char*> DeviceExtensions(VkInstance, VkPhysicalDevice) { return {}; }
void OnDeviceCreated(const VulkanInstance*, const VulkanDevice*) {}
bool StereoWanted() { return REXCVAR_GET(xr_simulate); }
bool NextEye(EyeRequest& out) {
  if (!REXCVAR_GET(xr_simulate)) return false;
  const uint64_t sequence = [] {
    std::lock_guard<std::mutex> lock(B().mutex);
    return B().sequence;
  }();
  const int eye = int(sequence & 1);
  const float half = float(REXCVAR_GET(xr_simulate_ipd)) * 0.5f;
  const float position[3] = {eye ? half : -half, 0.0f, 0.0f};
  const float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  out = Record(eye, position, orientation);
  return true;
}
bool BeginFrame(uint32_t, uint32_t) { return false; }
void RecordCopy(VkCommandBuffer, VkImage, uint32_t, uint32_t, VkImageLayout, VkAccessFlags,
                VkPipelineStageFlags, const Lens&) {}
void EndFrame() {}
void NoteLens(const Lens& lens) {
  EyeRequest request;
  FindRequest(lens, request);
  LogDecoding("simulated");
}

}  // namespace openxr
}  // namespace vulkan
}  // namespace ui
}  // namespace rex

#endif  // REX_HAS_OPENXR
