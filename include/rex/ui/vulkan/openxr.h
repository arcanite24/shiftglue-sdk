#pragma once
/**
 * @file        rex/ui/vulkan/openxr.h
 * @brief       The title's output in a VR headset through OpenXR.
 *
 * With xr_enabled, the Vulkan instance and device are created with the
 * extensions the player's OpenXR runtime asks for (XR_KHR_vulkan_enable), a
 * session opens on that device, and presented frames go to the headset:
 *
 * - On a screen: each frame is a quad in front of the player.
 * - In stereo (xr_stereo), for titles whose host moves their camera per
 *   frame: the host asks NextEye for each frame's eye and pose, renders it
 *   with that camera and tags the frame through its vertical field of view
 *   (TagFov). The frame's world-view-projection gives the field of view back
 *   (GuestOutputLens), FovTag recovers the request, and the frame goes to its
 *   eye of a projection layer; the other eye keeps its latest frame
 *   (alternate-eye rendering). Frames without a tag or a world (menus) show
 *   on the screen.
 *
 * Builds without REX_HAS_OPENXR (cmake/rexglue_openxr.cmake) compile these to
 * no-ops; xr_simulate runs the stereo bookkeeping without a headset.
 *
 * @license     BSD 3-Clause License
 */

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/xr.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;
class VulkanInstance;

namespace openxr {

// Extensions the runtime needs on the instance, and on this physical device.
// Empty when xr_enabled is off, the build has no OpenXR, or no runtime or
// headset answers. The names stay valid for the life of the process.
std::vector<const char*> InstanceExtensions();
std::vector<const char*> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical_device);

// Opens the session on the presenter's device, once.
void OnDeviceCreated(const VulkanInstance* instance, const VulkanDevice* device);

// Whether stereo frames are wanted: a running stereo session, or
// xr_simulate. The GPU side then measures each frame's lens.
bool StereoWanted();

// One frame's camera in the headset's start (LOCAL) space: x right, y up,
// -z forward, in metres, and the orientation as a unit quaternion (x, y, z,
// w). `tag` goes into the frame's field of view through TagFov.
struct EyeRequest {
  int eye = 0;  // 0 left, 1 right
  uint32_t tag = 0;
  float position[3] = {};
  float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};
// The next frame's eye and pose; the eyes alternate. Call once per title
// frame, from any thread; false (and nothing requested) while stereo is off.
bool NextEye(EyeRequest& out);

// The headset's vertical field of view (radians, the wider eye's), once
// NextEye has located the eyes; 0 before that and without a headset.
float HeadsetFovY();
// And horizontal (the wider eye's), for the host's render aspect.
float HeadsetFovX();
// The vertical field of view of the main view of the latest presented
// tagged frame (radians), or 0: lets the host tell its main camera from
// render-to-texture cameras.
float PresentedFovY();

// The tag in a vertical field of view (rex/ui/xr.h).
using xr::FovTag;
using xr::kTagCount;
using xr::TagFov;

// A presented frame's lens, from its world-view-projection: full horizontal
// and vertical fields of view in radians.
struct Lens {
  bool valid = false;
  float fov_x = 0.0f;
  float fov_y = 0.0f;
  // Where the camera was (the matrix's world units) and its right axis, for
  // checking that frames reach the eye they were rendered for.
  bool has_position = false;
  float position[3] = {};
  float right[3] = {};
};

// The presenter's per-frame hooks, all on its painting thread. BeginFrame
// waits for the headset's next frame and returns true when the frame wants
// an image; RecordCopy then copies the guest output (in the given layout,
// which it restores) into the eye the lens tags, or the screen, in the
// paint's command buffer. EndFrame goes after that command buffer is
// submitted, whether or not BeginFrame returned true.
bool BeginFrame(uint32_t width, uint32_t height);
void RecordCopy(VkCommandBuffer command_buffer, VkImage source, uint32_t width, uint32_t height,
                VkImageLayout source_layout, VkAccessFlags source_access,
                VkPipelineStageFlags source_stage, const Lens& lens);
void EndFrame();
// Painting without a headset frame (xr_simulate, or no session): counts the
// lens's tag for the log.
void NoteLens(const Lens& lens);

// Saves the next `frames` stereo frames as the headset gets them, into
// `directory`: each eye's image (xr-capture-N-left.ppm, ...) and a line of
// xr-capture.jsonl with what it was rendered with (tag, pose, field of view),
// the head pose the headset predicted for its display, and its age. Each
// saved frame waits for the GPU, so the game hitches while it runs.
void RequestCapture(const std::string& directory, int frames);

}  // namespace openxr
}  // namespace vulkan
}  // namespace ui
}  // namespace rex
