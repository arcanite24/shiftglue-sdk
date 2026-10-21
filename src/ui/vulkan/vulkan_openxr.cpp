/**
 * @file        ui/vulkan/vulkan_openxr.cpp
 * @brief       The title's output in a VR headset through OpenXR.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/vulkan/openxr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
REXCVAR_DEFINE_BOOL(xr_match_refresh, true, "Display",
                    "In VR, pace the game to the frames the headset asks for (its refresh, "
                    "or half of it with spacewarp) instead of the frame rate limit: one frame "
                    "per headset frame on the screen, two (a pair of eyes) in stereo")
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
REXCVAR_DEFINE_INT32(xr_log_frames, 0, "Display",
                     "Testing: log each stereo frame's decoded eye, tag and camera for this "
                     "many frames from when it is set")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(xr_simulate_hz, 45, "Display",
                     "Frames a second xr_simulate's headset asks for (rex/ui/xr.h "
                     "HeadsetFrameHz), as Virtual Desktop's spacewarp at 90 Hz does")
    .range(0, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(xr_test_eye_spread, 1.0, "Display",
                      "Testing: spread the headset's eyes this many times wider about the head's "
                      "centre, so captures show which eye each image was rendered for")
    .range(1.0, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(xr_test_yaw, 0.0, "Display",
                      "Testing: turn the headset's pose left and right this many degrees, once "
                      "every 2 s, on top of the real head, so a still headset captures turning")
    .range(0.0, 90.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(xr_pair_wait_when_behind, false, "Display",
                    "Keep waiting for the next eye pair, and nudging the game's frames into "
                    "phase, when the game is not giving a new pair every headset frame (off: "
                    "the headset takes the latest pair and does not wait)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(xr_shared_eye_pose, true, "Display",
                    "In VR stereo, the second eye of a pair takes the pose located with the "
                    "first; off, each eye locates its own")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(xr_right_eye_first, false, "Display",
                    "Testing: render each stereo pair's right eye first and its left second")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(xr_simulate_yaw, 0.0, "Display",
                      "Testing: xr_simulate's head turns left and right this many degrees, once "
                      "every 2 s")
    .range(0.0, 90.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(xr_simulate_fov, 1.6, "Display",
                      "Vertical field of view in radians xr_simulate reports for its headset")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex {
namespace ui {
namespace vulkan {
namespace openxr {

namespace {

// What NextEye handed out, by tag, for presented frames to find again.
struct Requested {
  bool valid = false;
  EyeRequest request;
  uint64_t requested_ns = 0;
};

uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

struct Bookkeeping {
  std::mutex mutex;
  uint64_t sequence = 0;
  Requested ring[kTagCount];
  // Log: frames decoded per eye, untagged, lens missing.
  uint64_t decoded[2] = {}, untagged = 0, no_lens = 0, last_logged = 0;
  // Eye check: between consecutive decoded frames, does the camera move
  // sideways the way their requests do? Frame to frame (the change of the
  // move, so steady car motion cancels), and within a pair (a left frame and
  // the right one requested after it): a host rendering a pair from one
  // moment moves the camera by the eyes' offset alone there, while turning
  // and sliding fool the frame to frame check.
  bool have_previous = false;
  Lens previous_lens;
  EyeRequest previous_request;
  uint64_t eye_match = 0, eye_mismatch = 0, detailed = 0, mismatches_logged = 0;
  uint64_t pair_match = 0, pair_mismatch = 0, pair_mismatches_logged = 0;
  // Change of the sideways move between consecutive frames, measured and
  // expected: steady car motion cancels out of it, the eyes' swap does not.
  double previous_moved = 0.0, previous_expected = 0.0;
  bool have_previous_move = false;
  // Pair check: from a left frame to the right frame requested after it,
  // how far the camera moved beyond the eyes' own offset (the world's
  // motion between the eyes, which shows as false depth).
  double pair_motion_sum = 0.0, pair_motion_max = 0.0;
  uint64_t pairs = 0;
  // Timing: how old a frame is when it is copied for the headset, and how
  // often each eye gets a new one.
  double age_sum_ms = 0.0, age_max_ms = 0.0, interval_sum_ms = 0.0;
  uint64_t timed = 0, intervals = 0, last_eye_ns[2] = {}, last_tag = UINT64_MAX;
  std::atomic<float> presented_fov_y{0.0f};
  // A decoded frame's age from its request to its copy, smoothed: NextEye
  // predicts the head that far ahead.
  std::atomic<float> recent_age_ms{0.0f};
  std::atomic<float> headset_fov_y{0.0f};
  std::atomic<float> headset_fov_x{0.0f};
};

Bookkeeping& B() {
  static Bookkeeping bookkeeping;
  return bookkeeping;
}

// The request a presented frame's lens tags, if any.
bool FindRequest(const Lens& lens, EyeRequest& out, uint64_t* requested_ns = nullptr) {
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
  if (requested_ns) *requested_ns = b.ring[tag].requested_ns;
  {
    static int32_t last_setting = 0, remaining = 0;
    const int32_t setting = REXCVAR_GET(xr_log_frames);
    if (setting != last_setting) remaining = setting;
    last_setting = setting;
    if (remaining > 0) {
      --remaining;
      const double qx = out.orientation[0], qy = out.orientation[1], qz = out.orientation[2],
                   qw = out.orientation[3];
      const double request_yaw =
          std::atan2(2.0 * (qw * qy + qx * qz), 1.0 - 2.0 * (qy * qy + qx * qx)) * 57.29577951308232;
      const double right_heading = std::atan2(lens.right[2], lens.right[0]) * 57.29577951308232;
      REXLOG_INFO("OpenXR frame log: tag {} eye {} camera ({:.4f}, {:.4f}, {:.4f}) right ({:.4f}, "
                  "{:.4f}, {:.4f}) requested yaw {:.3f} rendered right heading {:.3f}",
                  tag, out.eye, lens.position[0], lens.position[1], lens.position[2],
                  lens.right[0], lens.right[1], lens.right[2], request_yaw, right_heading);
    }
  }
  ++b.decoded[out.eye & 1];
  b.presented_fov_y = lens.fov_y;
  // A frame painted again (no new one yet) is not counted twice.
  const bool repeat = b.last_tag == tag;
  b.last_tag = tag;
  if (!repeat) {
    const uint64_t now = SteadyNs();
    const double age = double(now - b.ring[tag].requested_ns) / 1e6;
    b.age_sum_ms += age;
    const float recent = b.recent_age_ms.load(std::memory_order_relaxed);
    b.recent_age_ms.store(recent > 0.0f ? recent + 0.05f * (float(age) - recent) : float(age),
                          std::memory_order_relaxed);
    b.age_max_ms = std::max(b.age_max_ms, age);
    ++b.timed;
    uint64_t& last = b.last_eye_ns[out.eye & 1];
    if (last) {
      b.interval_sum_ms += double(now - last) / 1e6;
      ++b.intervals;
    }
    last = now;
  }
  if (!repeat && b.have_previous && lens.has_position && b.previous_lens.has_position) {
    double moved = 0.0;
    for (int i = 0; i < 3; ++i) {
      moved += double(lens.position[i] - b.previous_lens.position[i]) * lens.right[i];
    }
    // The eyes' offset along the head's right axis.
    const double x = out.orientation[0], y = out.orientation[1], z = out.orientation[2],
                 w = out.orientation[3];
    const double right[3] = {1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z),
                             2.0 * (x * z - w * y)};
    double expected = 0.0;
    for (int i = 0; i < 3; ++i) {
      expected += (double(out.position[i]) - b.previous_request.position[i]) * right[i];
    }
    if (b.have_previous_move) {
      const double change = moved - b.previous_moved;
      const double expected_change = expected - b.previous_expected;
      if (std::abs(expected_change) > 0.02) {
        const bool match = (change > 0.0) == (expected_change > 0.0);
        ++(match ? b.eye_match : b.eye_mismatch);
        if (!match && b.mismatches_logged < 20) {
          ++b.mismatches_logged;
          REXLOG_INFO(
              "OpenXR eye check: tag {} eye {} moved {:+.4f} (before {:+.4f}), expected {:+.4f} "
              "(before {:+.4f})",
              out.tag, out.eye, moved, b.previous_moved, expected, b.previous_expected);
        }
      }
    }
    if (out.eye == 1 - FirstEye() && b.previous_request.eye == FirstEye() &&
        out.tag == (b.previous_request.tag + 1) % kTagCount) {
      double residual = 0.0;
      for (int i = 0; i < 3; ++i) {
        const double d =
            double(lens.position[i] - b.previous_lens.position[i]) - expected * lens.right[i];
        residual += d * d;
      }
      residual = std::sqrt(residual);
      const bool match = std::abs(moved - expected) < std::abs(moved + expected);
      ++(match ? b.pair_match : b.pair_mismatch);
      if (!match && b.pair_mismatches_logged < 20) {
        ++b.pair_mismatches_logged;
        REXLOG_INFO("OpenXR pair check: tags {}-{} moved {:+.4f}, expected {:+.4f}",
                    b.previous_request.tag, out.tag, moved, expected);
      }
      b.pair_motion_sum += residual;
      b.pair_motion_max = std::max(b.pair_motion_max, residual);
      ++b.pairs;
    }
    b.previous_moved = moved;
    b.previous_expected = expected;
    b.have_previous_move = true;
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
      "eye check {} right, {} wrong; frame age {:.1f} ms (max {:.1f}), each eye every {:.1f} ms; "
      "world motion between paired eyes {:.1f} mm (max {:.1f}) over {} pairs; pair check {} "
      "right, {} wrong",
      where, b.sequence, b.decoded[0], b.decoded[1], b.untagged, b.no_lens, b.eye_match,
      b.eye_mismatch, b.timed ? b.age_sum_ms / double(b.timed) : 0.0, b.age_max_ms,
      b.intervals ? b.interval_sum_ms / double(b.intervals) : 0.0,
      b.pairs ? 1000.0 * b.pair_motion_sum / double(b.pairs) : 0.0, 1000.0 * b.pair_motion_max,
      b.pairs, b.pair_match, b.pair_mismatch);
  b.age_sum_ms = b.age_max_ms = b.interval_sum_ms = 0.0;
  b.timed = b.intervals = 0;
  b.pair_motion_sum = b.pair_motion_max = 0.0;
  b.pairs = 0;
}

// xr_simulate's head: turning about the vertical (xr_simulate_yaw), as an
// orientation quaternion.
// As the headset's eyes, a pair's two eyes share the left eye's moment.
void SimulatedOrientation(int eye, float orientation[4]) {
  static double yaw = 0.0;
  const double amplitude = REXCVAR_GET(xr_simulate_yaw) / 57.29577951308232;
  if (eye == FirstEye()) yaw = amplitude * std::sin(double(SteadyNs()) / 1e9 * 3.141592653589793);
  orientation[0] = 0.0f;
  orientation[1] = float(std::sin(yaw * 0.5));
  orientation[2] = 0.0f;
  orientation[3] = float(std::cos(yaw * 0.5));
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
  b.ring[request.tag] = {true, request, SteadyNs()};
  ++b.sequence;
  return request;
}

}  // namespace

int FirstEye() { return REXCVAR_GET(xr_right_eye_first) ? 1 : 0; }

float HeadsetFovY() {
  if (REXCVAR_GET(xr_simulate)) return float(REXCVAR_GET(xr_simulate_fov));
  return B().headset_fov_y.load(std::memory_order_relaxed);
}
float HeadsetFovX() {
  if (REXCVAR_GET(xr_simulate)) {
    // A Quest 3 eye is about 1.2 times as wide as tall.
    return float(2.0 * std::atan(std::tan(REXCVAR_GET(xr_simulate_fov) * 0.5) * 1.2));
  }
  return B().headset_fov_x.load(std::memory_order_relaxed);
}
float PresentedFovY() { return B().presented_fov_y.load(std::memory_order_relaxed); }

}  // namespace openxr
}  // namespace vulkan
}  // namespace ui
}  // namespace rex

REXCVAR_DEFINE_BOOL(xr_vulkan_enable2, true, "Display",
                    "In VR, let the runtime create the Vulkan instance and device "
                    "(XR_KHR_vulkan_enable2) when it offers that, so that its queue calls go "
                    "through the game's queue lock one by one and its frame calls need not hold "
                    "the game's queue. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(xr_frame_calls_hold_queue, false, "Display",
                    "In VR, hold the game's queue across the runtime's frame and swapchain "
                    "calls even when its own queue calls take the lock (diagnostics)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#if REX_HAS_OPENXR

#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <string>
#include <condition_variable>
#include <thread>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <fmt/format.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

namespace rex {
namespace ui {
namespace vulkan {
namespace openxr {

void FinishFenced(bool wait);

namespace {

// One swapchain the guest output is copied into: the screen, or an eye.
struct Target {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  std::vector<XrSwapchainImageVulkanKHR> images;
  bool acquired = false;
  // The acquired image is ready to write (xrWaitSwapchainImage returned).
  bool ready = false;
  uint32_t index = 0;
  // An image has been released since the swapchain was made: a layer may
  // only use a swapchain once it has one.
  bool released = false;
  // What the frame in the acquired image was rendered with (eyes).
  XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  XrFovf fov{};
  // The screen: its frame's width / height as shown (0: the pixels').
  float display_aspect = 0.0f;
};

enum TargetIndex { kLeft, kRight, kScreen, kTargetCount };

// What BeginGuestFrame's copy releases once it is on the queue.
enum class Pending { kNone, kPair, kScreen };

struct State {
  // Everything below, and the session's swapchain and frame calls.
  std::mutex mutex;
  bool instance_tried = false;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions = nullptr;
  PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions = nullptr;
  PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device = nullptr;
  PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements = nullptr;
  // XR_KHR_vulkan_enable2: the runtime creates the instance and the device.
  bool enable2 = false;
  PFN_xrCreateVulkanInstanceKHR create_vulkan_instance = nullptr;
  PFN_xrCreateVulkanDeviceKHR create_vulkan_device = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR get_graphics_device2 = nullptr;
  VkInstance runtime_instance = VK_NULL_HANDLE;
  VkDevice runtime_device = VK_NULL_HANDLE;
  // Extension names handed to Vulkan, kept for the life of the process.
  std::deque<std::string> names;

  const VulkanDevice* device = nullptr;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  XrSpace view = XR_NULL_HANDLE;
  // The seat: LOCAL moved to where the head was, facing level, at the start
  // and at each Recenter. Runtimes put LOCAL's origin in different places
  // (Virtual Desktop at the head at start, Meta's runtime where it was last
  // recentred, 2.2 m away in a test), and the eye poses go to the title's
  // camera as offsets from it. Spaces replaced while NextEye may use them
  // stay until the session ends.
  std::atomic<XrSpace> seat{XR_NULL_HANDLE};
  std::vector<XrSpace> old_seats;
  std::atomic<bool> recenter{true};
  // Headset frames the session has been visible for: the seat waits for
  // 30, since Virtual Desktop reports a placeholder head pose, (0, -0.963,
  // 0) m facing ahead, when the session starts.
  uint32_t visible_frames = 0;
  std::atomic<bool> running{false};
  // The headset shows the session (VISIBLE or FOCUSED). Otherwise (headset
  // off or asleep) the runtime stops taking images, and acquiring one would
  // wait out its timeout on every game frame.
  std::atomic<bool> visible{false};
  // The display time NextEye locates the eyes at: the latest frame's, one
  // period ahead (the game's frame shows about then).
  std::atomic<int64_t> next_display_time{0};
  std::atomic<int64_t> display_period{0};
  // NextEye: both eyes of a pair come from one xrLocateViews, at the time
  // the pair is expected on the headset; the right eye's view waits here.
  bool have_right_view = false;
  float right_position[3] = {};
  float right_orientation[4] = {};

  // The headset's frames: a thread of their own, at the headset's rate,
  // whatever the game's frame rate.
  std::thread thread;
  std::atomic<bool> stop{false};
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};

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

  // Pairing: a left eye's frame stays acquired (unseen) until the right one
  // requested after it is copied, then both are released together.
  uint32_t held_left_tag = UINT32_MAX;
  Pending pending = Pending::kNone;
  // A copy on the queue waits for this fence before its images go to the
  // runtime: Virtual Desktop read a right eye's image before its copy had
  // run (the left one, copied a frame earlier, always had), and the right
  // eye alone jittered.
  VkFence copy_fence = VK_NULL_HANDLE;
  Pending fenced = Pending::kNone;

  // Stereo pairs go through slots of our own: the game copies each eye into
  // one as it is drawn, and each headset frame copies the next whole pair
  // into the swapchains, acquired and released inside that frame, so both
  // eyes reach the runtime the same way at the same moment. Copying into
  // the swapchains from the game's swap (a left eye a frame before its
  // right, released together later) put the right eye a frame out from its
  // head pose in Virtual Desktop now and then: in a headset recording it
  // moved 0, 8, 16 or 24 px a frame as the head turned, the left a steady 8.
  struct Slot {
    VkImage eye[2] = {};
    VkDeviceMemory memory[2] = {};
    uint32_t width = 0, height = 0;
    XrPosef pose[2] = {};
    XrFovf fov[2] = {};
    uint32_t left_tag = UINT32_MAX;
    bool has_left = false, complete = false;
    VkFence fence = VK_NULL_HANDLE;
    uint64_t serial = 0, complete_ns = 0;
  };
  // Five: at 3x a pair is still being copied on the GPU when the next one
  // is filled, and with three the unshown one was overwritten before it was
  // ever shown (about a third of the pairs).
  static constexpr int kSlots = 5;
  Slot slots[kSlots];
  int filling = -1, shown_slot = -1;
  // The slot the headset thread copies from, outside the state mutex.
  int copying_slot = -1;
  uint64_t slot_serial = 0, shown_serial = 0;
  // The copy BeginGuestFrame set up completes the slot being filled.
  bool pending_slot = false;
  // The headset thread's copies into the swapchains.
  VkCommandPool xr_pool = VK_NULL_HANDLE;
  VkCommandBuffer xr_cmd[2] = {};
  VkFence xr_cmd_fence[2] = {};
  int xr_cmd_next = 0;
  // What the headset shows: the eyes' latest released pair, or the screen.
  bool show_stereo = false, show_screen = false;
  // Pairs released (GuestFrameSubmitted) and the latest shown, and when the
  // latest was released: the headset's frame takes the next pair, waiting
  // for it a little, and the guest's vblank is nudged so pairs arrive a
  // third of a frame after the headset's frame starts.
  std::condition_variable pair_released;
  uint64_t released_pairs = 0, shown_pairs = 0;
  uint64_t last_release_ns = 0;
  XrPosef shown_pose[2] = {};
  XrFovf shown_fov[2] = {};

  // RequestCapture: frames left to save, and the readback of this frame's.
  std::string capture_directory;
  int capture_left = 0;
  uint32_t capture_index = 0;
  VkBuffer capture_buffer = VK_NULL_HANDLE;
  VkDeviceMemory capture_memory = VK_NULL_HANDLE;
  VkDeviceSize capture_size = 0;
  bool capture_recorded = false;
  EyeRequest capture_request;
  Lens capture_lens;
  uint64_t capture_requested_ns = 0, capture_recorded_ns = 0;
  uint32_t capture_width = 0, capture_height = 0;

  // Log: headset frames, new pairs shown, right eyes dropped for lack of
  // their left, since the last line.
  std::chrono::steady_clock::time_point last_pose_log{};
  uint64_t frames = 0, pairs = 0, unpaired = 0;
  std::atomic<uint32_t> headset_hz{0};
  // Where the headset thread's time goes, and how long the game's copy
  // holds the state lock (acquiring a swapchain image waits in it).
  struct Timing {
    double sum_ms = 0.0, max_ms = 0.0;
    uint64_t count = 0;
    void Add(double ms) {
      sum_ms += ms;
      max_ms = std::max(max_ms, ms);
      ++count;
    }
    std::string Text() const {
      return fmt::format("{:.2f} ms (max {:.1f})", count ? sum_ms / double(count) : 0.0, max_ms);
    }
  } wait_frame, wait_lock, begin_end, guest_hold, release_hold;
  // When each shown pair arrived after its headset frame started (negative:
  // before), and the frames that showed the previous pair again.
  Timing pair_phase;
  uint64_t repeats = 0;
  // xr_pair_wait_when_behind: new pairs a headset frame (a running mean),
  // the released count at the last frame, and the frames the game kept up.
  double pair_rate = 1.0;
  uint64_t released_at_frame = 0;
  uint64_t kept_up_frames = 0;
};

// Never destroyed: the headset's thread may still run at exit.
State& S() {
  static State* state = new State;
  return *state;
}

// The runtime's Vulkan functions under XR_KHR_vulkan_enable2: it looks them up
// through RuntimeGetInstanceProcAddr, which hands out the queue calls wrapped
// to take the device's lock on that queue for the call alone. The game's
// threads use the same lock, so the runtime's frame calls need not hold the
// queue throughout: Virtual Desktop's xrEndFrame waits there for the queue to
// reach its signal, and the game could not submit meanwhile, so the GPU ran
// dry and the game fell behind for the rest of the session.
struct RuntimeDispatch {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkQueueSubmit queue_submit = nullptr;
  PFN_vkQueueSubmit2 queue_submit2 = nullptr;
  PFN_vkQueueSubmit2 queue_submit2_khr = nullptr;
  PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
  PFN_vkQueueBindSparse queue_bind_sparse = nullptr;
  std::atomic<const VulkanDevice*> device{nullptr};
  // Queue calls the runtime made through these: it uses this dispatch.
  std::atomic<uint64_t> calls{0};
};

RuntimeDispatch& D() {
  static RuntimeDispatch* dispatch = new RuntimeDispatch;
  return *dispatch;
}

std::unique_lock<std::recursive_mutex> LockRuntimeQueue(VkQueue queue) {
  if (!D().calls.fetch_add(1, std::memory_order_relaxed)) {
    REXLOG_INFO("OpenXR: the runtime's queue calls take the game's queue lock; its frame "
                "calls no longer hold the queue");
  }
  if (const VulkanDevice* device = D().device.load(std::memory_order_acquire)) {
    for (const VulkanDevice::QueueFamily& family : device->queue_families()) {
      for (const auto& candidate : family.queues) {
        if (candidate && candidate->queue == queue) {
          return std::unique_lock<std::recursive_mutex>(candidate->mutex);
        }
      }
    }
  }
  return {};
}

VKAPI_ATTR VkResult VKAPI_CALL RuntimeQueueSubmit(VkQueue queue, uint32_t count,
                                                  const VkSubmitInfo* submits, VkFence fence) {
  const auto lock = LockRuntimeQueue(queue);
  return D().queue_submit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL RuntimeQueueSubmit2(VkQueue queue, uint32_t count,
                                                   const VkSubmitInfo2* submits, VkFence fence) {
  const auto lock = LockRuntimeQueue(queue);
  return D().queue_submit2(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL RuntimeQueueSubmit2KHR(VkQueue queue, uint32_t count,
                                                      const VkSubmitInfo2* submits,
                                                      VkFence fence) {
  const auto lock = LockRuntimeQueue(queue);
  return D().queue_submit2_khr(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL RuntimeQueueWaitIdle(VkQueue queue) {
  const auto lock = LockRuntimeQueue(queue);
  return D().queue_wait_idle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL RuntimeQueueBindSparse(VkQueue queue, uint32_t count,
                                                      const VkBindSparseInfo* infos,
                                                      VkFence fence) {
  const auto lock = LockRuntimeQueue(queue);
  return D().queue_bind_sparse(queue, count, infos, fence);
}

// The wrapper for a queue function, keeping the real one; nullptr for others.
PFN_vkVoidFunction WrapQueueFunction(const char* name, PFN_vkVoidFunction real) {
  if (!real) return nullptr;
  RuntimeDispatch& d = D();
  if (!std::strcmp(name, "vkQueueSubmit")) {
    d.queue_submit = PFN_vkQueueSubmit(real);
    return PFN_vkVoidFunction(&RuntimeQueueSubmit);
  }
  if (!std::strcmp(name, "vkQueueSubmit2")) {
    d.queue_submit2 = PFN_vkQueueSubmit2(real);
    return PFN_vkVoidFunction(&RuntimeQueueSubmit2);
  }
  if (!std::strcmp(name, "vkQueueSubmit2KHR")) {
    d.queue_submit2_khr = PFN_vkQueueSubmit2(real);
    return PFN_vkVoidFunction(&RuntimeQueueSubmit2KHR);
  }
  if (!std::strcmp(name, "vkQueueWaitIdle")) {
    d.queue_wait_idle = PFN_vkQueueWaitIdle(real);
    return PFN_vkVoidFunction(&RuntimeQueueWaitIdle);
  }
  if (!std::strcmp(name, "vkQueueBindSparse")) {
    d.queue_bind_sparse = PFN_vkQueueBindSparse(real);
    return PFN_vkVoidFunction(&RuntimeQueueBindSparse);
  }
  return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL RuntimeGetDeviceProcAddr(VkDevice device,
                                                                 const char* name) {
  const PFN_vkVoidFunction real = D().get_device_proc_addr(device, name);
  const PFN_vkVoidFunction wrapped = WrapQueueFunction(name, real);
  return wrapped ? wrapped : real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL RuntimeGetInstanceProcAddr(VkInstance instance,
                                                                   const char* name) {
  const PFN_vkVoidFunction real = D().get_instance_proc_addr(instance, name);
  if (real && !std::strcmp(name, "vkGetDeviceProcAddr")) {
    D().get_device_proc_addr = PFN_vkGetDeviceProcAddr(real);
    return PFN_vkVoidFunction(&RuntimeGetDeviceProcAddr);
  }
  const PFN_vkVoidFunction wrapped = WrapQueueFunction(name, real);
  return wrapped ? wrapped : real;
}

// The game's queue across one of the runtime's calls, unless the runtime's own
// queue calls take the lock (it has made one through RuntimeDispatch).
std::optional<VulkanDevice::Queue::Acquisition> HoldQueueForRuntime() {
  State& s = S();
  std::optional<VulkanDevice::Queue::Acquisition> hold;
  if (!s.enable2 || !D().calls.load(std::memory_order_relaxed) ||
      REXCVAR_GET(xr_frame_calls_hold_queue)) {
    hold.emplace(s.device->AcquireQueue(s.device->queue_family_graphics_compute(), 0));
  }
  return hold;
}

// The space eye poses, layers and logs use: the seat once set, else LOCAL.
XrSpace Seat() {
  const XrSpace seat = S().seat.load(std::memory_order_acquire);
  return seat != XR_NULL_HANDLE ? seat : S().local;
}

bool Check(XrResult result, const char* what);

// Under the state mutex: the seat where the head is at `time`, facing its
// direction about the vertical. False until the head is tracked.
bool PlaceSeat(XrTime time) {
  State& s = S();
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(s.view, s.local, time, &head)) ||
      !(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
      !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
    return false;
  }
  const XrQuaternionf& q = head.pose.orientation;
  // The head's forward (-z) direction, and the yaw that turns -z onto it.
  const double fx = -2.0 * (q.x * q.z + q.w * q.y);
  const double fz = -(1.0 - 2.0 * (q.x * q.x + q.y * q.y));
  const double yaw = std::atan2(-fx, -fz);
  XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  info.poseInReferenceSpace.orientation = {0.0f, float(std::sin(yaw * 0.5)), 0.0f,
                                           float(std::cos(yaw * 0.5))};
  info.poseInReferenceSpace.position = head.pose.position;
  XrSpace seat = XR_NULL_HANDLE;
  if (!Check(xrCreateReferenceSpace(s.session, &info, &seat), "creating the seat space")) {
    return false;
  }
  if (XrSpace old = s.seat.exchange(seat); old != XR_NULL_HANDLE) s.old_seats.push_back(old);
  REXLOG_INFO("OpenXR: seat set at ({:.3f}, {:.3f}, {:.3f}) m, facing {:.1f} degrees",
              head.pose.position.x, head.pose.position.y, head.pose.position.z,
              yaw * 57.29577951308232);
  return true;
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
  bool has_vulkan = false, has_vulkan2 = false;
  {
    std::string names;
    for (const auto& extension : extensions) {
      names += names.empty() ? "" : " ";
      names += extension.extensionName;
    }
    REXLOG_INFO("OpenXR: runtime extensions: {}", names);
  }
  for (const auto& extension : extensions) {
    has_vulkan |= std::strcmp(extension.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
    has_vulkan2 |= std::strcmp(extension.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
  }
  s.enable2 = has_vulkan2 && REXCVAR_GET(xr_vulkan_enable2);
  if (!has_vulkan && !s.enable2) {
    REXLOG_WARN("OpenXR: the runtime has no {}; VR is off", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    return false;
  }

  XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(create_info.applicationInfo.applicationName, "ReXGlue title",
               XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(create_info.applicationInfo.engineName, "ReXGlue", XR_MAX_ENGINE_NAME_SIZE - 1);
  create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  const char* enabled[] = {s.enable2 ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
                                    : XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
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
  const bool procs =
      s.enable2
          ? Proc(s.instance, "xrCreateVulkanInstanceKHR", s.create_vulkan_instance) &&
                Proc(s.instance, "xrCreateVulkanDeviceKHR", s.create_vulkan_device) &&
                Proc(s.instance, "xrGetVulkanGraphicsDevice2KHR", s.get_graphics_device2) &&
                Proc(s.instance, "xrGetVulkanGraphicsRequirements2KHR",
                     s.get_graphics_requirements)
          : Proc(s.instance, "xrGetVulkanInstanceExtensionsKHR", s.get_instance_extensions) &&
                Proc(s.instance, "xrGetVulkanDeviceExtensionsKHR", s.get_device_extensions) &&
                Proc(s.instance, "xrGetVulkanGraphicsDeviceKHR", s.get_graphics_device) &&
                Proc(s.instance, "xrGetVulkanGraphicsRequirementsKHR",
                     s.get_graphics_requirements);
  if (!Check(xrGetSystem(s.instance, &system_info, &s.system),
             "finding a headset (is it connected and awake?)") ||
      !procs) {
    xrDestroyInstance(s.instance);
    s.instance = XR_NULL_HANDLE;
    return false;
  }
  XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(xrGetSystemProperties(s.instance, s.system, &system_properties))) {
    REXLOG_INFO("OpenXR: headset {}", system_properties.systemName);
  }
  REXLOG_INFO("OpenXR: Vulkan through {}", s.enable2 ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
                                                    : XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
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

// Under the state mutex.
void DestroySession() {
  State& s = S();
  // NextEye (the game thread) stops using the session first.
  s.running = false;
  s.visible = false;
  xr::SetHeadsetRefreshHz(0);
  xr::SetHeadsetFrameHz(0);
  s.next_display_time = 0;
  for (Target& target : s.targets) DestroyTarget(target);
  s.held_left_tag = UINT32_MAX;
  s.pending = Pending::kNone;
  s.fenced = Pending::kNone;
  s.pending_slot = false;
  s.filling = s.shown_slot = -1;
  s.shown_serial = s.slot_serial;
  for (State::Slot& slot : s.slots) slot.complete = slot.has_left = false;
  s.show_stereo = s.show_screen = false;
  if (s.view != XR_NULL_HANDLE) xrDestroySpace(s.view);
  if (XrSpace seat = s.seat.exchange(XR_NULL_HANDLE); seat != XR_NULL_HANDLE) xrDestroySpace(seat);
  for (XrSpace seat : s.old_seats) xrDestroySpace(seat);
  s.old_seats.clear();
  s.recenter = true;
  if (s.local != XR_NULL_HANDLE) xrDestroySpace(s.local);
  if (s.session != XR_NULL_HANDLE) xrDestroySession(s.session);
  s.view = s.local = XR_NULL_HANDLE;
  s.session = XR_NULL_HANDLE;
  s.running = false;
}

// Under the state mutex.
void PollEvents() {
  State& s = S();
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (s.instance != XR_NULL_HANDLE && xrPollEvent(s.instance, &event) == XR_SUCCESS) {
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
      REXLOG_INFO("OpenXR: session state {}", int(changed.state));
      s.visible = changed.state == XR_SESSION_STATE_VISIBLE ||
                  changed.state == XR_SESSION_STATE_FOCUSED;
      if (!s.visible) s.visible_frames = 0;
      if (changed.state == XR_SESSION_STATE_READY && !s.running) {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        s.running = Check(xrBeginSession(s.session, &begin), "xrBeginSession");
      } else if (changed.state == XR_SESSION_STATE_STOPPING && s.running) {
        s.running = false;
        xr::SetHeadsetRefreshHz(0);
        xr::SetHeadsetFrameHz(0);
        xrEndSession(s.session);
      } else if (changed.state == XR_SESSION_STATE_EXITING ||
                 changed.state == XR_SESSION_STATE_LOSS_PENDING) {
        REXLOG_INFO("OpenXR: the runtime ended the session; VR is off until restart");
        DestroySession();
      }
    } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
      // The headset's own recentre moved LOCAL: seat the player again once
      // it has (the seat is placed a few frames later).
      REXLOG_INFO("OpenXR: the headset recentred; placing the seat again");
      s.recenter = true;
      s.visible_frames = 0;
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

// Acquires and waits for an image without stalling the game: a runtime that
// is not taking frames (Virtual Desktop losing the headset) held each game
// frame for the full wait. On a timeout the image stays acquired and the
// next frame waits for it again.
bool Acquire(Target& target, int64_t timeout_ns = 2'000'000) {
  State& s = S();
  if (target.acquired) {
    if (target.ready) return true;
    XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    image_wait.timeout = timeout_ns;
    const XrResult result = xrWaitSwapchainImage(target.swapchain, &image_wait);
    target.ready = result == XR_SUCCESS;
    return target.ready;
  }
  {
    // The runtime may use the game's queue here.
    const auto queue = HoldQueueForRuntime();
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(target.swapchain, &acquire_info, &target.index),
               "xrAcquireSwapchainImage")) {
      return false;
    }
  }
  target.acquired = true;
  XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  image_wait.timeout = timeout_ns;
  const XrResult result = xrWaitSwapchainImage(target.swapchain, &image_wait);
  target.ready = result == XR_SUCCESS;
  if (XR_FAILED(result)) Check(result, "xrWaitSwapchainImage");
  return target.ready;
}

// With the queue held.
void Release(Target& target) {
  if (!target.acquired || !target.ready) return;
  target.acquired = false;
  target.ready = false;
  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  target.released |=
      Check(xrReleaseSwapchainImage(target.swapchain, &release_info), "xrReleaseSwapchainImage");
}

// Under the state mutex: the slot's images at this size (8-bit UNORM, like
// the staging image; they end each game copy as color attachments).
bool EnsureSlot(State::Slot& slot, uint32_t width, uint32_t height) {
  State& s = S();
  if (slot.eye[0] != VK_NULL_HANDLE && slot.width == width && slot.height == height) return true;
  for (int eye = 0; eye < 2; ++eye) {
    if (slot.eye[eye] != VK_NULL_HANDLE) s.retired.emplace_back(slot.eye[eye], slot.memory[eye]);
    slot.eye[eye] = VK_NULL_HANDLE;
  }
  for (int eye = 0; eye < 2; ++eye) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = s.staging_format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!util::CreateDedicatedAllocationImage(s.device, image_info,
                                              util::MemoryPurpose::kDeviceLocal, slot.eye[eye],
                                              slot.memory[eye])) {
      REXLOG_WARN("OpenXR: could not create a {}x{} eye image", width, height);
      slot.eye[eye] = VK_NULL_HANDLE;
      return false;
    }
  }
  slot.width = width;
  slot.height = height;
  return true;
}

// Under the state mutex: the newest whole pair not shown yet whose copy has
// run, or -1.
int ReadySlot() {
  State& s = S();
  int best = -1;
  for (int i = 0; i < State::kSlots; ++i) {
    const State::Slot& slot = s.slots[i];
    if (!slot.complete || slot.serial <= s.shown_serial || slot.fence == VK_NULL_HANDLE) continue;
    if (s.device->functions().vkGetFenceStatus(s.device->device(), slot.fence) != VK_SUCCESS) {
      continue;
    }
    if (best < 0 || slot.serial > s.slots[best].serial) best = i;
  }
  return best;
}

// Under the state mutex: a slot for a new pair. Never the shown or copying
// one, nor the newest pair not yet shown; a free or stale slot first, then
// the oldest pair not yet shown.
int FillSlot() {
  State& s = S();
  int newest = -1;
  for (int i = 0; i < State::kSlots; ++i) {
    const State::Slot& slot = s.slots[i];
    if (slot.complete && slot.serial > s.shown_serial &&
        (newest < 0 || slot.serial > s.slots[newest].serial)) {
      newest = i;
    }
  }
  int oldest = -1;
  for (int i = 0; i < State::kSlots; ++i) {
    if (i == s.shown_slot || i == newest || i == s.copying_slot) continue;
    const State::Slot& slot = s.slots[i];
    if (!slot.complete || slot.serial <= s.shown_serial) return i;
    if (oldest < 0 || slot.serial < s.slots[oldest].serial) oldest = i;
  }
  return oldest;
}

// Under the state mutex, on the headset thread inside its frame: copies the
// slot's pair into the eye swapchains and releases both.
bool CopySlot(State::Slot& slot) {
  State& s = S();
  const VulkanDevice::Functions& dfn = s.device->functions();
  const VkDevice device = s.device->device();
  Target& left = s.targets[kLeft];
  Target& right = s.targets[kRight];
  if (!EnsureTarget(left, slot.width, slot.height, "left eye") ||
      !EnsureTarget(right, slot.width, slot.height, "right eye") ||
      !Acquire(left, 20'000'000) || !Acquire(right, 20'000'000)) {
    return false;
  }
  if (s.xr_pool == VK_NULL_HANDLE) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = s.device->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &s.xr_pool) != VK_SUCCESS) {
      s.xr_pool = VK_NULL_HANDLE;
      return false;
    }
    VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = s.xr_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 2;
    dfn.vkAllocateCommandBuffers(device, &allocate_info, s.xr_cmd);
    for (VkFence& fence : s.xr_cmd_fence) {
      VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      dfn.vkCreateFence(device, &fence_info, nullptr, &fence);
    }
  }
  const int c = s.xr_cmd_next;
  s.xr_cmd_next ^= 1;
  dfn.vkWaitForFences(device, 1, &s.xr_cmd_fence[c], VK_TRUE, 100'000'000);
  dfn.vkResetFences(device, 1, &s.xr_cmd_fence[c]);
  const VkCommandBuffer cmd = s.xr_cmd[c];
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(cmd, &begin_info);
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
  const VkImage targets[2] = {left.images[left.index].image, right.images[right.index].image};
  VkImageMemoryBarrier before[4];
  for (int eye = 0; eye < 2; ++eye) {
    before[eye] =
        barrier(slot.eye[eye], VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    before[2 + eye] = barrier(targets[eye], 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  }
  dfn.vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_TRANSFER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 4, before);
  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.dstSubresource = copy.srcSubresource;
  copy.extent = {slot.width, slot.height, 1};
  for (int eye = 0; eye < 2; ++eye) {
    dfn.vkCmdCopyImage(cmd, slot.eye[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets[eye],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  }
  VkImageMemoryBarrier after[4];
  for (int eye = 0; eye < 2; ++eye) {
    after[eye] = barrier(targets[eye], VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // Back as the game's copy leaves its images.
    after[2 + eye] = barrier(slot.eye[eye], VK_ACCESS_TRANSFER_READ_BIT, 0,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 4, after);
  dfn.vkEndCommandBuffer(cmd);
  const auto queue = s.device->AcquireQueue(s.device->queue_family_graphics_compute(), 0);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, s.xr_cmd_fence[c]) != VK_SUCCESS) {
    // Signalled for the next use.
    dfn.vkQueueSubmit(queue.queue(), 0, nullptr, s.xr_cmd_fence[c]);
    return false;
  }
  Release(left);
  Release(right);
  return true;
}

bool EnsureCaptureBuffer(uint32_t width, uint32_t height) {
  State& s = S();
  const VulkanDevice::Functions& dfn = s.device->functions();
  const VkDeviceSize size = VkDeviceSize(width) * height * 4;
  if (s.capture_buffer != VK_NULL_HANDLE && s.capture_size == size) return true;
  if (s.capture_buffer != VK_NULL_HANDLE) {
    // Only written by a frame that was waited for (GuestFrameSubmitted).
    dfn.vkDestroyBuffer(s.device->device(), s.capture_buffer, nullptr);
    dfn.vkFreeMemory(s.device->device(), s.capture_memory, nullptr);
    s.capture_buffer = VK_NULL_HANDLE;
  }
  if (!util::CreateDedicatedAllocationBuffer(s.device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             util::MemoryPurpose::kReadback, s.capture_buffer,
                                             s.capture_memory)) {
    REXLOG_WARN("OpenXR: could not create the capture buffer");
    s.capture_buffer = VK_NULL_HANDLE;
    s.capture_left = 0;
    return false;
  }
  s.capture_size = size;
  return true;
}

// After the GPU finished the captured frame: its image and what it was
// rendered with, beside the head pose the headset predicted for its display.
void SaveCapture() {
  State& s = S();
  const VulkanDevice::Functions& dfn = s.device->functions();
  void* mapped = nullptr;
  if (dfn.vkMapMemory(s.device->device(), s.capture_memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
      VK_SUCCESS) {
    s.capture_left = 0;
    return;
  }
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = s.capture_memory;
  range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(s.device->device(), 1, &range);
  const EyeRequest& r = s.capture_request;
  const uint32_t index = s.capture_index++;
  const std::string name = std::string("xr-capture-") + std::to_string(index) +
                           (r.eye ? "-right.ppm" : "-left.ppm");
  {
    std::ofstream image(s.capture_directory + "/" + name, std::ios::binary);
    image << "P6\n" << s.capture_width << ' ' << s.capture_height << "\n255\n";
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    const bool bgra = s.staging_format == VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<uint8_t> row(size_t(s.capture_width) * 3);
    for (uint32_t y = 0; y < s.capture_height; ++y) {
      const uint8_t* in = pixels + size_t(y) * s.capture_width * 4;
      for (uint32_t x = 0; x < s.capture_width; ++x) {
        row[x * 3 + 0] = in[x * 4 + (bgra ? 2 : 0)];
        row[x * 3 + 1] = in[x * 4 + 1];
        row[x * 3 + 2] = in[x * 4 + (bgra ? 0 : 2)];
      }
      image.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
  }
  dfn.vkUnmapMemory(s.device->device(), s.capture_memory);
  XrSpaceLocation shown{XR_TYPE_SPACE_LOCATION};
  xrLocateSpace(s.view, Seat(), s.frame_state.predictedDisplayTime, &shown);
  const XrPosef& h = shown.pose;
  std::ofstream log(s.capture_directory + "/xr-capture.jsonl", std::ios::app);
  log << fmt::format(
      "{{\"index\":{},\"image\":\"{}\",\"eye\":{},\"tag\":{},"
      "\"rendered_position\":[{:.4f},{:.4f},{:.4f}],\"rendered_orientation\":[{:.4f},{:.4f},{:.4f},{:.4f}],"
      "\"fov_x\":{:.5f},\"fov_y\":{:.5f},\"age_ms\":{:.2f},"
      "\"shown_head_position\":[{:.4f},{:.4f},{:.4f}],\"shown_head_orientation\":[{:.4f},{:.4f},{:.4f},{:.4f}],"
      "\"display_period_ms\":{:.2f},"
      "\"camera_position\":[{:.4f},{:.4f},{:.4f}],\"camera_right\":[{:.4f},{:.4f},{:.4f}]}}\n",
      index, name, r.eye, r.tag, r.position[0], r.position[1], r.position[2], r.orientation[0],
      r.orientation[1], r.orientation[2], r.orientation[3], s.capture_lens.fov_x,
      s.capture_lens.fov_y, double(s.capture_recorded_ns - s.capture_requested_ns) / 1e6,
      h.position.x, h.position.y, h.position.z, h.orientation.x, h.orientation.y,
      h.orientation.z, h.orientation.w, double(s.frame_state.predictedDisplayPeriod) / 1e6,
      s.capture_lens.position[0], s.capture_lens.position[1], s.capture_lens.position[2],
      s.capture_lens.right[0], s.capture_lens.right[1], s.capture_lens.right[2]);
  if (--s.capture_left == 0) {
    REXLOG_INFO("OpenXR: capture saved ({} frames) in {}", s.capture_index, s.capture_directory);
  }
}

// Under the state mutex.
void LogPose() {
  State& s = S();
  const auto now = std::chrono::steady_clock::now();
  if (now - s.last_pose_log < std::chrono::seconds(5)) return;
  s.last_pose_log = now;
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(s.view, Seat(), s.frame_state.predictedDisplayTime, &location))) {
    return;
  }
  const XrPosef& p = location.pose;
  REXLOG_INFO(
      "OpenXR: head at ({:.3f}, {:.3f}, {:.3f}) m, orientation ({:.3f}, {:.3f}, {:.3f}, {:.3f}); "
      "headset asks for {} Hz, game paced at {} Hz; "
      "{} headset frames, {} eye pairs shown, {} right eyes without their left; headset thread "
      "waits {} for the headset, {} for the lock and the next pair, {} in begin and end; the game's copy holds "
      "the lock {}, its release {}; pairs arrive {} into the headset's frame, {} frames "
      "repeated a pair, the game kept up on {}",
      p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y,
      p.orientation.z, p.orientation.w, s.headset_hz.load(), xr::HeadsetRefreshHz(), s.frames,
      s.pairs, s.unpaired, s.wait_frame.Text(),
      s.wait_lock.Text(), s.begin_end.Text(), s.guest_hold.Text(), s.release_hold.Text(),
      s.pair_phase.Text(), s.repeats, s.kept_up_frames);
  s.pair_phase = {};
  s.repeats = 0;
  s.kept_up_frames = 0;
  s.frames = s.pairs = s.unpaired = 0;
  s.wait_frame = s.wait_lock = s.begin_end = s.guest_hold = s.release_hold = {};
}

// The headset's frame loop: waits for each of its frames and submits what it
// shows, the latest released pair or screen, so the headset gets a frame
// every refresh even when the game runs slower.
void FrameLoop() {
  State& s = S();
  // A failing runtime is logged at first and then once a second, and the
  // loop backs off, so it cannot flood the log.
  uint64_t failures = 0;
  auto failed = [&](XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
      failures = 0;
      return false;
    }
    if (failures++ % 100 < 3) Check(result, what);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return true;
  };
  while (!s.stop.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (s.session == XR_NULL_HANDLE) return;
      PollEvents();
    }
    if (!s.running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    const auto wait_start = std::chrono::steady_clock::now();
    if (failed(xrWaitFrame(s.session, &wait_info, &frame_state), "xrWaitFrame")) continue;
    const auto wait_end = std::chrono::steady_clock::now();
    // The guest's frame pacing follows the headset (rex/ui/xr.h): in stereo
    // two frames, a pair, per headset frame. The headset's frame is the one
    // it asks the app for: with spacewarp (Virtual Desktop's SSW) half its
    // refresh, the runtime making the frames between.
    const uint32_t headset_hz =
        frame_state.predictedDisplayPeriod > 0
            ? uint32_t(std::lround(1e9 / double(frame_state.predictedDisplayPeriod)))
            : 0u;
    s.headset_hz = headset_hz;
    xr::SetHeadsetFrameHz(headset_hz);
    xr::SetHeadsetRefreshHz(REXCVAR_GET(xr_match_refresh)
                                ? headset_hz * (s.show_stereo ? 2u : 1u)
                                : 0u);
    s.next_display_time = frame_state.predictedDisplayTime + frame_state.predictedDisplayPeriod;
    s.display_period = frame_state.predictedDisplayPeriod;

    std::unique_lock<std::mutex> lock(s.mutex);
    if (s.session == XR_NULL_HANDLE) return;
    // The next pair, in order: a pair that lands on the headset's frame
    // boundary otherwise shows twice and its successor not at all, which
    // read as constant judder at full frame rate.
    const int64_t period = frame_state.predictedDisplayPeriod;
    FinishFenced(false);
    int next = ReadySlot();
    // Whether the game gives a new pair every headset frame (a mean over about
    // 20 frames). Behind it (at 3x, about 60 pairs a second for 72 frames),
    // waiting for a pair that is not coming delays the headset's frame and
    // lets the game overwrite pairs never shown: take what is ready instead.
    s.pair_rate = s.pair_rate * 0.95 + double(s.slot_serial - s.released_at_frame) * 0.05;
    s.released_at_frame = s.slot_serial;
    const bool kept_up = s.pair_rate >= 0.95;
    if (kept_up) ++s.kept_up_frames;
    if (s.show_stereo && period > 0 && (kept_up || REXCVAR_GET(xr_pair_wait_when_behind))) {
      const uint64_t wait_ns = SteadyNs();
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::nanoseconds(period * 6 / 10);
      while (next < 0 && !s.stop && std::chrono::steady_clock::now() < deadline) {
        s.pair_released.wait_for(lock, std::chrono::microseconds(500));
        next = ReadySlot();
      }
      if (next < 0) {
        ++s.repeats;
      } else {
        // Keep pairs arriving a third of a frame in (a small step at a time).
        const int64_t phase = int64_t(s.slots[next].complete_ns) - int64_t(wait_ns);
        s.pair_phase.Add(double(phase) / 1e6);
        int64_t error = phase - period / 3;
        error = std::clamp<int64_t>(error, -period / 2, period / 2);
        xr::NudgeVblankPhase(-error / 8);
      }
    } else if (s.show_stereo && period > 0 && next < 0) {
      ++s.repeats;
    }
    const auto locked = std::chrono::steady_clock::now();
    s.wait_frame.Add(std::chrono::duration<double, std::milli>(wait_end - wait_start).count());
    s.wait_lock.Add(std::chrono::duration<double, std::milli>(locked - wait_end).count());
    s.frame_state = frame_state;
    if (s.visible) ++s.visible_frames;
    if (s.recenter.load(std::memory_order_acquire) && s.visible_frames >= 30 &&
        PlaceSeat(frame_state.predictedDisplayTime)) {
      s.recenter = false;
    }
    // The runtime's calls below can stall (Virtual Desktop losing the
    // headset took 100-200 ms in begin and end): the state mutex is not held
    // across them, or the game's copies wait on it and the game stalls. The
    // headset thread alone uses the eye swapchains, and the slot it copies
    // stays out of FillSlot's reach meanwhile.
    s.copying_slot = next;
    lock.unlock();
    bool begun = false;
    {
      const auto queue = HoldQueueForRuntime();
      XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
      begun = !failed(xrBeginFrame(s.session, &begin_info), "xrBeginFrame");
    }
    // The next pair into the swapchains, inside this frame.
    const bool copied = begun && next >= 0 && CopySlot(s.slots[next]);
    lock.lock();
    s.copying_slot = -1;
    if (!begun) continue;
    if (copied) {
      State::Slot& slot = s.slots[next];
      for (int eye = 0; eye < 2; ++eye) {
        s.shown_pose[eye] = slot.pose[eye];
        s.shown_fov[eye] = slot.fov[eye];
      }
      s.shown_slot = next;
      s.shown_serial = slot.serial;
      s.show_stereo = true;
      ++s.pairs;
    }
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[1];
    uint32_t layer_count = 0;
    if (frame_state.shouldRender && s.show_stereo && s.targets[kLeft].released &&
        s.targets[kRight].released) {
      for (int eye = 0; eye < 2; ++eye) {
        const Target& t = s.targets[eye];
        views[eye].pose = s.shown_pose[eye];
        views[eye].fov = s.shown_fov[eye];
        views[eye].subImage.swapchain = t.swapchain;
        views[eye].subImage.imageRect = {{0, 0}, {int32_t(t.width), int32_t(t.height)}};
      }
      projection.space = Seat();
      projection.viewCount = 2;
      projection.views = views;
      layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    } else if (frame_state.shouldRender && s.show_screen && s.targets[kScreen].released) {
      const Target& screen = s.targets[kScreen];
      const float width_m = float(REXCVAR_GET(xr_screen_width));
      quad.space = Seat();
      quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      quad.subImage.swapchain = screen.swapchain;
      quad.subImage.imageRect = {{0, 0}, {int32_t(screen.width), int32_t(screen.height)}};
      quad.pose.orientation.w = 1.0f;
      quad.pose.position.z = -float(REXCVAR_GET(xr_screen_distance));
      const float aspect = screen.display_aspect > 0.0f
                               ? screen.display_aspect
                               : float(screen.width) / float(screen.height);
      quad.size = {width_m, width_m / aspect};
      layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = layer_count;
    end_info.layers = layers;
    // The screen's swapchain is the game thread's: the layer is ended under
    // the mutex then, the eyes' without it.
    const bool screen_layer = layer_count && layers[0] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    if (!screen_layer) lock.unlock();
    XrResult ended;
    {
      const auto queue = HoldQueueForRuntime();
      ended = xrEndFrame(s.session, &end_info);
      if (XR_FAILED(ended) && layer_count) {
        // The frame is still open: end it without layers.
        end_info.layerCount = 0;
        xrEndFrame(s.session, &end_info);
      }
    }
    if (!lock.owns_lock()) lock.lock();
    if (!failed(ended, "xrEndFrame") && layer_count) ++s.frames;
    s.begin_end.Add(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - locked).count());
    LogPose();
  }
}

// xr::Shutdown, before a hard exit: the headset thread stops and the session
// and instance end, without waiting on the GPU (stragglers may hold it).
void ShutdownForExit() {
  State& s = S();
  s.stop = true;
  if (s.thread.joinable()) s.thread.join();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.session != XR_NULL_HANDLE) DestroySession();
  if (s.instance != XR_NULL_HANDLE) {
    xrDestroyInstance(s.instance);
    s.instance = XR_NULL_HANDLE;
  }
  REXLOG_INFO("OpenXR: session ended for exit");
}

}  // namespace

std::vector<const char*> InstanceExtensions() {
  std::lock_guard<std::mutex> lock(S().mutex);
  // XR_KHR_vulkan_enable2: the runtime adds its own in CreateInstance.
  if (!EnsureInstance() || S().enable2) return {};
  std::vector<const char*> names = Names(ExtensionList(S().get_instance_extensions));
  for (const char* name : names) REXLOG_INFO("OpenXR: the runtime wants instance extension {}", name);
  return names;
}

std::vector<const char*> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical_device) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  if (s.instance == XR_NULL_HANDLE || s.enable2) return {};
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

bool CreateInstance(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                    const VkInstanceCreateInfo& create_info, VkInstance& instance_out,
                    VkResult& result_out) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  if (!EnsureInstance() || !s.enable2) return false;
  // Asked for before the instance, as XR_KHR_vulkan_enable2 requires.
  XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  if (!Check(s.get_graphics_requirements(s.instance, s.system, &requirements),
             "xrGetVulkanGraphicsRequirements2KHR")) {
    return false;
  }
  D().get_instance_proc_addr = get_instance_proc_addr;
  XrVulkanInstanceCreateInfoKHR xr_info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xr_info.systemId = s.system;
  xr_info.pfnGetInstanceProcAddr = &RuntimeGetInstanceProcAddr;
  xr_info.vulkanCreateInfo = &create_info;
  if (!Check(s.create_vulkan_instance(s.instance, &xr_info, &instance_out, &result_out),
             "xrCreateVulkanInstanceKHR")) {
    return false;
  }
  if (result_out == VK_SUCCESS) s.runtime_instance = instance_out;
  return true;
}

bool CreateDevice(VkInstance instance, VkPhysicalDevice physical_device,
                  const VkDeviceCreateInfo& create_info, VkDevice& device_out,
                  VkResult& result_out) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  if (s.instance == XR_NULL_HANDLE || !s.enable2 || instance != s.runtime_instance) return false;
  // Only on the GPU the headset is on; another stays the caller's (VR off).
  XrVulkanGraphicsDeviceGetInfoKHR get_info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  get_info.systemId = s.system;
  get_info.vulkanInstance = instance;
  VkPhysicalDevice wanted = VK_NULL_HANDLE;
  if (!Check(s.get_graphics_device2(s.instance, &get_info, &wanted), "finding its GPU") ||
      wanted != physical_device) {
    return false;
  }
  XrVulkanDeviceCreateInfoKHR xr_info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xr_info.systemId = s.system;
  xr_info.pfnGetInstanceProcAddr = &RuntimeGetInstanceProcAddr;
  xr_info.vulkanPhysicalDevice = physical_device;
  xr_info.vulkanCreateInfo = &create_info;
  if (!Check(s.create_vulkan_device(s.instance, &xr_info, &device_out, &result_out),
             "xrCreateVulkanDeviceKHR")) {
    return false;
  }
  if (result_out == VK_SUCCESS) s.runtime_device = device_out;
  return true;
}

void OnDeviceCreated(const VulkanInstance* instance, const VulkanDevice* device) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  if (s.instance == XR_NULL_HANDLE || s.session != XR_NULL_HANDLE || !device) return;
  VkPhysicalDevice wanted = VK_NULL_HANDLE;
  if (s.enable2) {
    // The runtime made the device (CreateDevice) only on the headset's GPU.
    if (device->device() == s.runtime_device) wanted = device->physical_device();
  } else {
    s.get_graphics_device(s.instance, s.system, instance->instance(), &wanted);
  }
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
  // XR_KHR_vulkan_enable2 asked for them before the instance.
  XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  if (!s.enable2 && !Check(s.get_graphics_requirements(s.instance, s.system, &requirements),
                          "xrGetVulkanGraphicsRequirementsKHR")) {
    return;
  }
  // XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR is the same structure and type.
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  binding.instance = instance->instance();
  binding.physicalDevice = device->physical_device();
  binding.device = device->device();
  binding.queueFamilyIndex = device->queue_family_graphics_compute();
  binding.queueIndex = 0;
  // The runtime's queue calls from here on take this device's queue lock.
  D().device.store(device, std::memory_order_release);
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
  s.stop = false;
  s.thread = std::thread(FrameLoop);
  xr::SetShutdown(&ShutdownForExit);
  REXLOG_INFO("OpenXR: session created; put the headset on");
}

void OnDeviceDestroying() {
  State& s = S();
  s.stop = true;
  if (s.thread.joinable()) s.thread.join();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.session == XR_NULL_HANDLE) return;
  FinishFenced(true);
  const VulkanDevice::Functions& dfn = s.device->functions();
  {
    const auto queue = s.device->AcquireQueue(s.device->queue_family_graphics_compute(), 0);
    for (Target& target : s.targets) Release(target);
    // The staging and capture copies are done before they go.
    dfn.vkQueueWaitIdle(queue.queue());
  }
  DestroySession();
  D().device.store(nullptr, std::memory_order_release);
  if (s.staging != VK_NULL_HANDLE) s.retired.emplace_back(s.staging, s.staging_memory);
  for (const auto& [image, memory] : s.retired) {
    dfn.vkDestroyImage(s.device->device(), image, nullptr);
    dfn.vkFreeMemory(s.device->device(), memory, nullptr);
  }
  s.retired.clear();
  s.staging = VK_NULL_HANDLE;
  if (s.capture_buffer != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(s.device->device(), s.capture_buffer, nullptr);
    dfn.vkFreeMemory(s.device->device(), s.capture_memory, nullptr);
    s.capture_buffer = VK_NULL_HANDLE;
  }
  if (s.copy_fence != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(s.device->device(), s.copy_fence, nullptr);
    s.copy_fence = VK_NULL_HANDLE;
  }
  for (State::Slot& slot : s.slots) {
    for (int eye = 0; eye < 2; ++eye) {
      if (slot.eye[eye] != VK_NULL_HANDLE) {
        dfn.vkDestroyImage(s.device->device(), slot.eye[eye], nullptr);
        dfn.vkFreeMemory(s.device->device(), slot.memory[eye], nullptr);
        slot.eye[eye] = VK_NULL_HANDLE;
      }
    }
    if (slot.fence != VK_NULL_HANDLE) dfn.vkDestroyFence(s.device->device(), slot.fence, nullptr);
    slot.fence = VK_NULL_HANDLE;
  }
  for (VkFence& fence : s.xr_cmd_fence) {
    if (fence != VK_NULL_HANDLE) dfn.vkDestroyFence(s.device->device(), fence, nullptr);
    fence = VK_NULL_HANDLE;
  }
  if (s.xr_pool != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(s.device->device(), s.xr_pool, nullptr);
    s.xr_pool = VK_NULL_HANDLE;
  }
  s.device = nullptr;
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
  const int eye = int(sequence & 1) ^ FirstEye();
  if (REXCVAR_GET(xr_simulate)) {
    xr::SetHeadsetFrameHz(uint32_t(REXCVAR_GET(xr_simulate_hz)));
    const float half = float(REXCVAR_GET(xr_simulate_ipd)) * 0.5f;
    const float position[3] = {eye ? half : -half, 0.0f, 0.0f};
    float orientation[4];
    SimulatedOrientation(eye, orientation);
    out = Record(eye, position, orientation);
    return true;
  }
  State& s = S();
  if (!REXCVAR_GET(xr_stereo) || !s.running.load(std::memory_order_relaxed)) return false;
  if (eye != FirstEye() && s.have_right_view && REXCVAR_GET(xr_shared_eye_pose)) {
    // The pair's second eye, located with its first.
    s.have_right_view = false;
    out = Record(eye, s.right_position, s.right_orientation);
    return true;
  }
  s.have_right_view = false;
  // xrLocateViews may run beside the headset thread's frame calls. The pair
  // reaches the headset about a frame's age after its request (rendering,
  // the right eye, the copy): the head is predicted for then, so the
  // headset's correction for the head's motion since stays small.
  int64_t time = s.next_display_time.load(std::memory_order_relaxed);
  if (time == 0) return false;
  const int64_t period = s.display_period.load(std::memory_order_relaxed);
  const double age_ms = std::min(double(B().recent_age_ms.load(std::memory_order_relaxed)), 100.0);
  if (period > 0 && age_ms > 0.0) {
    time += int64_t(std::llround(age_ms * 1e6 / double(period))) * period;
  }
  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = time;
  locate_info.space = Seat();
  XrViewState view_state{XR_TYPE_VIEW_STATE};
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  uint32_t count = 0;
  if (XR_FAILED(xrLocateViews(s.session, &locate_info, &view_state, 2, &count, views)) ||
      count != 2 || !(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
      !(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
    return false;
  }
  float fov_y = 0.0f, fov_x = 0.0f;
  for (const XrView& view : views) {
    fov_y = std::max(fov_y, view.fov.angleUp - view.fov.angleDown);
    fov_x = std::max(fov_x, view.fov.angleRight - view.fov.angleLeft);
  }
  B().headset_fov_y = fov_y;
  B().headset_fov_x = fov_x;
  if (const double amplitude = REXCVAR_GET(xr_test_yaw); amplitude > 0.0) {
    // About the vertical through the head's centre, in the seat.
    const double yaw = amplitude / 57.29577951308232 *
                       std::sin(double(SteadyNs()) / 1e9 * 3.141592653589793);
    const float c = float(std::cos(yaw)), sn = float(std::sin(yaw));
    const XrQuaternionf turn{0.0f, float(std::sin(yaw * 0.5)), 0.0f, float(std::cos(yaw * 0.5))};
    XrVector3f& a = views[0].pose.position;
    XrVector3f& b = views[1].pose.position;
    const XrVector3f centre{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    for (XrView& view : views) {
      XrVector3f& p = view.pose.position;
      const float dx = p.x - centre.x, dz = p.z - centre.z;
      p.x = centre.x + c * dx + sn * dz;
      p.z = centre.z - sn * dx + c * dz;
      const XrQuaternionf q = view.pose.orientation;
      view.pose.orientation = {turn.w * q.x + turn.x * q.w + turn.y * q.z - turn.z * q.y,
                               turn.w * q.y - turn.x * q.z + turn.y * q.w + turn.z * q.x,
                               turn.w * q.z + turn.x * q.y - turn.y * q.x + turn.z * q.w,
                               turn.w * q.w - turn.x * q.x - turn.y * q.y - turn.z * q.z};
    }
  }
  if (const double spread = REXCVAR_GET(xr_test_eye_spread); spread != 1.0) {
    XrVector3f& a = views[0].pose.position;
    XrVector3f& b = views[1].pose.position;
    const XrVector3f centre{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    for (XrVector3f* p : {&a, &b}) {
      p->x = float(centre.x + (p->x - centre.x) * spread);
      p->y = float(centre.y + (p->y - centre.y) * spread);
      p->z = float(centre.z + (p->z - centre.z) * spread);
    }
  }
  const XrPosef& pose = views[eye].pose;
  const float position[3] = {pose.position.x, pose.position.y, pose.position.z};
  const float orientation[4] = {pose.orientation.x, pose.orientation.y, pose.orientation.z,
                                pose.orientation.w};
  if (eye == FirstEye()) {
    const XrPosef& right = views[1 - eye].pose;
    s.right_position[0] = right.position.x;
    s.right_position[1] = right.position.y;
    s.right_position[2] = right.position.z;
    s.right_orientation[0] = right.orientation.x;
    s.right_orientation[1] = right.orientation.y;
    s.right_orientation[2] = right.orientation.z;
    s.right_orientation[3] = right.orientation.w;
    s.have_right_view = true;
  }
  out = Record(eye, position, orientation);
  return true;
}

// Under the state mutex: once the fenced copy has run, hands its images to
// the runtime and shows them. With `wait`, waits for it (up to 100 ms).
void FinishFenced(bool wait) {
  State& s = S();
  if (s.fenced == Pending::kNone || s.copy_fence == VK_NULL_HANDLE) return;
  const VulkanDevice::Functions& dfn = s.device->functions();
  const VkResult status =
      wait ? dfn.vkWaitForFences(s.device->device(), 1, &s.copy_fence, VK_TRUE, 100'000'000)
           : dfn.vkGetFenceStatus(s.device->device(), s.copy_fence);
  if (status != VK_SUCCESS) return;
  const auto queue = s.device->AcquireQueue(s.device->queue_family_graphics_compute(), 0);
  if (s.fenced == Pending::kPair) {
    Release(s.targets[kLeft]);
    Release(s.targets[kRight]);
    for (int eye = 0; eye < 2; ++eye) {
      s.shown_pose[eye] = s.targets[eye].pose;
      s.shown_fov[eye] = s.targets[eye].fov;
    }
    s.show_stereo = true;
    s.held_left_tag = UINT32_MAX;
    ++s.pairs;
    ++s.released_pairs;
    s.last_release_ns = SteadyNs();
  } else if (s.fenced == Pending::kScreen) {
    Release(s.targets[kScreen]);
    s.show_stereo = false;
    s.show_screen = true;
  }
  s.fenced = Pending::kNone;
}

bool BeginGuestFrame(uint32_t width, uint32_t height, float display_aspect, const Lens& lens,
                     FrameCopy& out) {
  out = {};
  EyeRequest request;
  uint64_t requested_ns = 0;
  State& s = S();
  bool tagged = false;
  if (StereoWanted()) {
    tagged = FindRequest(lens, request, &requested_ns);
    LogDecoding(s.running.load(std::memory_order_relaxed) ? "stereo" : "simulated");
  }
  if (width == 0 || height == 0) return false;
  std::lock_guard<std::mutex> lock(s.mutex);
  // The images of the copy before go to the runtime before these are used.
  FinishFenced(true);
  struct Hold {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    State::Timing& timing;
    ~Hold() {
      timing.Add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                     .count());
    }
  } hold{std::chrono::steady_clock::now(), s.guest_hold};
  if (!s.running || !s.visible || s.session == XR_NULL_HANDLE || !s.device || !ChooseFormat() ||
      !EnsureStaging(width, height)) {
    return false;
  }
  const bool stereo = REXCVAR_GET(xr_stereo) && tagged;
  if (stereo) {
    // Into a slot; the headset's frame copies the whole pair on.
    State::Slot* slot = nullptr;
    if (request.eye == FirstEye()) {
      const int i = s.filling >= 0 ? s.filling : FillSlot();
      if (i < 0 || !EnsureSlot(s.slots[i], width, height)) return false;
      s.filling = i;
      slot = &s.slots[i];
      slot->has_left = true;
      slot->complete = false;
      slot->left_tag = request.tag;
    } else {
      if (s.filling < 0 || !s.slots[s.filling].has_left ||
          s.slots[s.filling].left_tag != (request.tag + kTagCount - 1) % kTagCount ||
          s.slots[s.filling].width != width || s.slots[s.filling].height != height) {
        // Its left frame was not copied (a camera change, or the first
        // frame): the headset keeps the last pair.
        ++s.unpaired;
        return false;
      }
      slot = &s.slots[s.filling];
      s.pending_slot = true;
    }
    const int eye = request.eye & 1;
    slot->pose[eye].position = {request.position[0], request.position[1], request.position[2]};
    slot->pose[eye].orientation = {request.orientation[0], request.orientation[1],
                                   request.orientation[2], request.orientation[3]};
    const float half_x = lens.fov_x * 0.5f, half_y = lens.fov_y * 0.5f;
    slot->fov[eye] = {-half_x, half_x, half_y, -half_y};
    if (s.capture_left > 0 && EnsureCaptureBuffer(width, height)) {
      out.capture = s.capture_buffer;
      s.capture_recorded = true;
      s.capture_request = request;
      s.capture_lens = lens;
      s.capture_requested_ns = requested_ns;
      s.capture_recorded_ns = SteadyNs();
      s.capture_width = width;
      s.capture_height = height;
    }
    out.staging = s.staging;
    out.target = slot->eye[eye];
    return true;
  }
  const TargetIndex index = kScreen;
  static const char* const kNames[] = {"left eye", "right eye", "screen"};
  Target& target = s.targets[index];
  if (!EnsureTarget(target, width, height, kNames[index]) || !Acquire(target)) return false;
  if (target.display_aspect != display_aspect) {
    REXLOG_INFO("OpenXR: screen {}x{} shown at {:.3f}:1", width, height,
                display_aspect > 0.0f ? display_aspect : float(width) / float(height));
    target.display_aspect = display_aspect;
  }
  if (stereo) {
    target.pose.position = {request.position[0], request.position[1], request.position[2]};
    target.pose.orientation = {request.orientation[0], request.orientation[1],
                               request.orientation[2], request.orientation[3]};
    const float half_x = lens.fov_x * 0.5f, half_y = lens.fov_y * 0.5f;
    target.fov = {-half_x, half_x, half_y, -half_y};
    if (s.capture_left > 0 && EnsureCaptureBuffer(width, height)) {
      out.capture = s.capture_buffer;
      s.capture_recorded = true;
      s.capture_request = request;
      s.capture_lens = lens;
      s.capture_requested_ns = requested_ns;
      s.capture_recorded_ns = SteadyNs();
      s.capture_width = width;
      s.capture_height = height;
    }
  }
  if (index == kLeft) {
    s.held_left_tag = request.tag;
  } else {
    s.pending = index == kRight ? Pending::kPair : Pending::kScreen;
  }
  out.staging = s.staging;
  out.target = target.images[target.index].image;
  return true;
}

void GuestFrameSubmitted() {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.session == XR_NULL_HANDLE) return;
  const auto start = std::chrono::steady_clock::now();
  const VulkanDevice::Functions& dfn = s.device->functions();
  {
    const auto queue = s.device->AcquireQueue(s.device->queue_family_graphics_compute(), 0);
    if (s.pending_slot && s.filling >= 0) {
      // The pair is whole once the GPU has run its copy (ReadySlot).
      State::Slot& slot = s.slots[s.filling];
      if (slot.fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        dfn.vkCreateFence(s.device->device(), &fence_info, nullptr, &slot.fence);
      } else {
        dfn.vkResetFences(s.device->device(), 1, &slot.fence);
      }
      dfn.vkQueueSubmit(queue.queue(), 0, nullptr, slot.fence);
      slot.complete = true;
      slot.has_left = false;
      slot.serial = ++s.slot_serial;
      slot.complete_ns = SteadyNs();
      s.filling = -1;
    }
    s.pending_slot = false;
    if (s.pending != Pending::kNone) {
      // Signalled once the GPU has run everything on the queue, the copy
      // included; FinishFenced then hands the images over.
      if (s.copy_fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        dfn.vkCreateFence(s.device->device(), &fence_info, nullptr, &s.copy_fence);
      } else {
        dfn.vkResetFences(s.device->device(), 1, &s.copy_fence);
      }
      dfn.vkQueueSubmit(queue.queue(), 0, nullptr, s.copy_fence);
      s.fenced = s.pending;
      s.pending = Pending::kNone;
    }
    if (s.capture_recorded) {
      s.capture_recorded = false;
      // Still holding the queue: wait for this frame's copy, then save it.
      dfn.vkQueueWaitIdle(queue.queue());
      SaveCapture();
    }
  }
  s.release_hold.Add(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  s.pair_released.notify_all();
}

void Recenter() { S().recenter = true; }

void RequestCapture(const std::string& directory, int frames) {
  std::lock_guard<std::mutex> lock(S().mutex);
  State& s = S();
  s.capture_directory = directory;
  s.capture_left = frames;
  REXLOG_INFO("OpenXR: capturing the next {} stereo frames into {}", frames, directory);
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
bool CreateInstance(PFN_vkGetInstanceProcAddr, const VkInstanceCreateInfo&, VkInstance&,
                    VkResult&) {
  return false;
}
bool CreateDevice(VkInstance, VkPhysicalDevice, const VkDeviceCreateInfo&, VkDevice&, VkResult&) {
  return false;
}
void OnDeviceCreated(const VulkanInstance*, const VulkanDevice*) {}
void OnDeviceDestroying() {}
bool StereoWanted() { return REXCVAR_GET(xr_simulate); }
bool NextEye(EyeRequest& out) {
  if (!REXCVAR_GET(xr_simulate)) return false;
  const uint64_t sequence = [] {
    std::lock_guard<std::mutex> lock(B().mutex);
    return B().sequence;
  }();
  const int eye = int(sequence & 1) ^ FirstEye();
  xr::SetHeadsetFrameHz(uint32_t(REXCVAR_GET(xr_simulate_hz)));
  const float half = float(REXCVAR_GET(xr_simulate_ipd)) * 0.5f;
  const float position[3] = {eye ? half : -half, 0.0f, 0.0f};
  float orientation[4];
  SimulatedOrientation(eye, orientation);
  out = Record(eye, position, orientation);
  return true;
}
bool BeginGuestFrame(uint32_t, uint32_t, float, const Lens& lens, FrameCopy& out) {
  out = {};
  if (!REXCVAR_GET(xr_simulate)) return false;
  EyeRequest request;
  FindRequest(lens, request);
  LogDecoding("simulated");
  return false;
}
void GuestFrameSubmitted() {}
void Recenter() {}
void RequestCapture(const std::string&, int) {}

}  // namespace openxr
}  // namespace vulkan
}  // namespace ui
}  // namespace rex

#endif  // REX_HAS_OPENXR
