#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/submission_tracker.h>
#include <rex/ui/vulkan/ui_samplers.h>
#include <rex/ui/vulkan/upscaler.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanUIDrawContext final : public UIDrawContext {
 public:
  VulkanUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                      uint32_t render_target_height, VkCommandBuffer draw_command_buffer,
                      uint64_t submission_index_current, uint64_t submission_index_completed,
                      VkRenderPass render_pass, VkFormat render_pass_format)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        draw_command_buffer_(draw_command_buffer),
        submission_index_current_(submission_index_current),
        submission_index_completed_(submission_index_completed),
        render_pass_(render_pass),
        render_pass_format_(render_pass_format) {}

  VkCommandBuffer draw_command_buffer() const { return draw_command_buffer_; }
  uint64_t submission_index_current() const { return submission_index_current_; }
  uint64_t submission_index_completed() const { return submission_index_completed_; }
  VkRenderPass render_pass() const { return render_pass_; }
  VkFormat render_pass_format() const { return render_pass_format_; }

 private:
  VkCommandBuffer draw_command_buffer_;
  uint64_t submission_index_current_;
  uint64_t submission_index_completed_;
  // Has 1 subpass with a single render_pass_format_ attachment.
  VkRenderPass render_pass_;
  VkFormat render_pass_format_;
};

// From the latest swapchain: whether its surface offers scRGB, and whether
// it is in use (present_hdr).
struct HdrOutputState {
  bool offered = false;
  bool active = false;
};
HdrOutputState GetHdrOutputState();

class VulkanPresenter final : public Presenter {
 public:
  // Maximum number of different guest output image versions still potentially
  // considered alive that may be given to the refresher - this many instances
  // of dependent objects (such as framebuffers) may need to be kept by the
  // refresher across invocations (due to multiple-buffering of guest output
  // images inside the presenter, different versions may be given even every
  // invocation), to avoid recreation of dependent objects every frame.
  static constexpr size_t kMaxActiveGuestOutputImageVersions = kGuestOutputMailboxSize;

  static constexpr VkFormat kGuestOutputFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  // The guest output is expected to be acquired and released in this state by
  // the refresher. The exception is the first write to the current guest output
  // image - in this case, a barrier is only needed from
  // VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT without access. Also if the image is
  // being refreshed for the first time, it's in VK_IMAGE_LAYOUT_UNDEFINED (but
  // it's safe, and preferred, to transition it from VK_IMAGE_LAYOUT_UNDEFINED
  // when writing to it in general).
  static constexpr VkPipelineStageFlagBits kGuestOutputInternalStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  static constexpr VkAccessFlags kGuestOutputInternalAccessMask = VK_ACCESS_SHADER_READ_BIT;
  static constexpr VkImageLayout kGuestOutputInternalLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Defined below; refresh contexts point at one.
 private:
  class GuestOutputImage;

 public:
  // The callback must use the graphics and compute queue 0 of the device.
  class VulkanGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    VulkanGuestOutputRefreshContext(bool& is_8bpc_out_ref, VkImage image, VkImageView image_view,
                                    uint64_t image_version, bool image_ever_written_previously,
                                    GuestOutputImage* guest_output_image = nullptr,
                                    bool* hudless_written_out = nullptr,
                                    UpscalerCamera* camera_out = nullptr,
                                    bool* camera_written_out = nullptr)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          image_(image),
          image_view_(image_view),
          image_version_(image_version),
          image_ever_written_previously_(image_ever_written_previously),
          guest_output_image_(guest_output_image),
          hudless_written_out_(hudless_written_out),
          camera_out_(camera_out),
          camera_written_out_(camera_written_out) {}

    // The format is kGuestOutputFormat.
    // Supports usage as a color attachment and as a sampled image, as well as
    // transfer source (but the reason of that is guest output capturing).
    VkImage image() const { return image_; }
    VkImageView image_view() const { return image_view_; }
    uint64_t image_version() const { return image_version_; }
    // Whether a proper barrier must be done to acquire the image.
    bool image_ever_written_previously() const { return image_ever_written_previously_; }

    // UP-1.2, when WantsHudlessGuestOutput: the guest output without the
    // game's HUD, with the HUD's coverage (0 or 1) in alpha. Same format,
    // usage and release state as the guest output; created on first use.
    // After writing it, the refresher calls SetHudlessWritten, or painting
    // ignores it for this frame.
    bool EnsureHudlessImage();
    VkImage hudless_image() const;
    VkImageView hudless_image_view() const;
    bool hudless_ever_written_previously() const;
    void SetHudlessWritten();

    // UP-1.3 and UP-1.4, when WantsGuestOutputCamera: the scene depth (guest
    // z in [0, 1], kGuestOutputDepthFormat) and the motion of each pixel to
    // its place in the previous frame (in UV units, kGuestOutputMotionFormat),
    // at the guest output's size. Sampled and storage usage; released to
    // kGuestOutputInternal* like the guest output. Created on first use.
    bool EnsureCameraImages();
    VkImage depth_image() const;
    VkImageView depth_image_view() const;
    VkImage motion_image() const;
    VkImageView motion_image_view() const;
    bool camera_ever_written_previously() const;
    void SetCameraWritten(const UpscalerCamera& camera);

   private:
    VkImage image_;
    VkImageView image_view_;
    uint64_t image_version_;
    bool image_ever_written_previously_;
    GuestOutputImage* guest_output_image_;
    bool* hudless_written_out_;
    UpscalerCamera* camera_out_;
    bool* camera_written_out_;
  };

  static std::unique_ptr<VulkanPresenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                 const VulkanDevice* vulkan_device,
                                                 const UISamplers* ui_samplers) {
    auto presenter = std::unique_ptr<VulkanPresenter>(
        new VulkanPresenter(host_gpu_loss_callback, vulkan_device, ui_samplers));
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~VulkanPresenter();

  const VulkanDevice* vulkan_device() const { return vulkan_device_; }

  static Surface::TypeFlags GetSurfaceTypesSupportedByInstance(
      const VulkanInstance::Extensions& instance_extensions);
  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;
  bool CapturePresentedOutput(RawImage& image_out) override;
  std::vector<UpscalerInfo> GetUpscalers() const override;
  bool WantsHudlessGuestOutput() const override {
    return hudless_guest_output_wanted_.load(std::memory_order_relaxed);
  }
  bool WantsGuestOutputCamera() const override {
    return guest_output_camera_wanted_.load(std::memory_order_relaxed);
  }
  uint32_t GuestOutputJitterPhases() const override {
    return guest_output_jitter_phases_.load(std::memory_order_relaxed);
  }
  // The formats of the guest output's camera images.
  static constexpr VkFormat kGuestOutputDepthFormat = VK_FORMAT_R32_SFLOAT;
  static constexpr VkFormat kGuestOutputMotionFormat = VK_FORMAT_R16G16_SFLOAT;

  void AwaitUISubmissionCompletionFromUIThread(uint64_t submission_index) {
    ui_submission_tracker_.AwaitSubmissionCompletion(submission_index);
  }
  VkCommandBuffer AcquireUISetupCommandBufferFromUIThread();

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  void GuestOutputRefreshSubmitted(uint32_t mailbox_index) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  // Usable for both the guest output image itself and for intermediate images.
  class GuestOutputImage {
   public:
    static std::unique_ptr<GuestOutputImage> Create(const VulkanDevice* const vulkan_device,
                                                    const uint32_t width, const uint32_t height) {
      assert_not_zero(width);
      assert_not_zero(height);
      auto image =
          std::unique_ptr<GuestOutputImage>(new GuestOutputImage(vulkan_device, width, height));
      if (!image->Initialize()) {
        return nullptr;
      }
      return std::move(image);
    }

    GuestOutputImage(const GuestOutputImage& image) = delete;
    GuestOutputImage& operator=(const GuestOutputImage& image) = delete;
    ~GuestOutputImage();

    const VkExtent2D& extent() const { return extent_; }

    VkImage image() const { return image_; }
    VkDeviceMemory memory() const { return memory_; }
    VkImageView view() const { return view_; }

    // The HUD-less copy (VulkanGuestOutputRefreshContext::EnsureHudlessImage).
    bool EnsureHudless();
    VkImage hudless_image() const { return hudless_image_; }
    VkImageView hudless_view() const { return hudless_view_; }
    bool hudless_ever_written = false;
    // The camera images (VulkanGuestOutputRefreshContext::EnsureCameraImages).
    bool EnsureCamera();
    VkImage depth_image() const { return depth_image_; }
    VkImageView depth_view() const { return depth_view_; }
    VkImage motion_image() const { return motion_image_; }
    VkImageView motion_view() const { return motion_view_; }
    bool camera_ever_written = false;

   private:
    GuestOutputImage(const VulkanDevice* const vulkan_device, const uint32_t width,
                     const uint32_t height)
        : vulkan_device_(vulkan_device) {
      extent_.width = width;
      extent_.height = height;
    }

    bool Initialize();
    bool CreateImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view,
                     VkFormat format = kGuestOutputFormat,
                     VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_STORAGE_BIT);

    const VulkanDevice* vulkan_device_;

    VkExtent2D extent_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkImage hudless_image_ = VK_NULL_HANDLE;
    VkDeviceMemory hudless_memory_ = VK_NULL_HANDLE;
    VkImageView hudless_view_ = VK_NULL_HANDLE;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkImage motion_image_ = VK_NULL_HANDLE;
    VkDeviceMemory motion_memory_ = VK_NULL_HANDLE;
    VkImageView motion_view_ = VK_NULL_HANDLE;
  };

  struct GuestOutputImageInstance {
    // Refresher-side reference (painting has its own references for the purpose
    // of destruction only after painting is done on the GPU).
    std::shared_ptr<GuestOutputImage> image;
    uint64_t version = UINT64_MAX;
    // Which refresh wrote the image last (guest_output_refresh_serial_).
    uint64_t refresh_serial = 0;
    // Whether that refresh also wrote the HUD-less copy, and the camera
    // images with this camera.
    bool hudless_valid = false;
    bool camera_valid = false;
    UpscalerCamera camera;
    uint64_t last_refresher_submission = 0;
    // For choosing the barrier stage and access mask and layout depending on
    // whether the image has previously been written. If an image is active
    // after a refresh, it can be assumed that this is true.
    bool ever_successfully_refreshed = false;

    void SetToNewImage(const std::shared_ptr<GuestOutputImage>& new_image, uint64_t new_version) {
      image = new_image;
      version = new_version;
      last_refresher_submission = 0;
      ever_successfully_refreshed = false;
      hudless_valid = false;
      camera_valid = false;
    }
  };

  struct GuestOutputPaintRectangleConstants {
    union {
      struct {
        float x;
        float y;
      };
      float offset[2];
    };
    union {
      struct {
        float width;
        float height;
      };
      float size[2];
    };
  };

  enum GuestOutputPaintPipelineLayoutIndex : size_t {
    kGuestOutputPaintPipelineLayoutIndexBilinear,
#if defined(REX_HAS_FIDELITYFX_SDK)
    kGuestOutputPaintPipelineLayoutIndexCasSharpen,
    kGuestOutputPaintPipelineLayoutIndexCasResample,
    kGuestOutputPaintPipelineLayoutIndexFsrEasu,
    kGuestOutputPaintPipelineLayoutIndexFsrRcas,
#endif
    // present_hdr's last pass (HdrOutputConstants).
    kGuestOutputPaintPipelineLayoutIndexHdr,

    kGuestOutputPaintPipelineLayoutCount,
  };

  static constexpr GuestOutputPaintPipelineLayoutIndex GetGuestOutputPaintPipelineLayoutIndex(
      GuestOutputPaintEffect effect) {
    switch (effect) {
      case GuestOutputPaintEffect::kBilinear:
      case GuestOutputPaintEffect::kBilinearDither:
        return kGuestOutputPaintPipelineLayoutIndexBilinear;
#if defined(REX_HAS_FIDELITYFX_SDK)
      case GuestOutputPaintEffect::kCasSharpen:
      case GuestOutputPaintEffect::kCasSharpenDither:
        return kGuestOutputPaintPipelineLayoutIndexCasSharpen;
      case GuestOutputPaintEffect::kCasResample:
      case GuestOutputPaintEffect::kCasResampleDither:
        return kGuestOutputPaintPipelineLayoutIndexCasResample;
      case GuestOutputPaintEffect::kFsrEasu:
        return kGuestOutputPaintPipelineLayoutIndexFsrEasu;
      case GuestOutputPaintEffect::kFsrRcas:
      case GuestOutputPaintEffect::kFsrRcasDither:
        return kGuestOutputPaintPipelineLayoutIndexFsrRcas;
#endif
      default:
        assert_unhandled_case(effect);
        return kGuestOutputPaintPipelineLayoutCount;
    }
  }

  struct PaintContext {
    class Submission {
     public:
      static std::unique_ptr<Submission> Create(const VulkanDevice* const vulkan_device) {
        auto submission = std::unique_ptr<Submission>(new Submission(vulkan_device));
        if (!submission->Initialize()) {
          return nullptr;
        }
        return submission;
      }

      Submission(const Submission& submission) = delete;
      Submission& operator=(const Submission& submission) = delete;
      ~Submission();

      VkSemaphore acquire_semaphore() const { return acquire_semaphore_; }
      VkCommandPool draw_command_pool() const { return draw_command_pool_; }
      VkCommandBuffer draw_command_buffer() const { return draw_command_buffer_; }

     private:
      explicit Submission(const VulkanDevice* const vulkan_device)
          : vulkan_device_(vulkan_device) {}
      bool Initialize();

      const VulkanDevice* vulkan_device_;
      VkSemaphore acquire_semaphore_ = VK_NULL_HANDLE;
      VkCommandPool draw_command_pool_ = VK_NULL_HANDLE;
      VkCommandBuffer draw_command_buffer_ = VK_NULL_HANDLE;
    };

    static constexpr uint32_t kSubmissionCount = 3;

    struct GuestOutputPaintPipeline {
      // Created on initialization.
      VkPipeline intermediate_pipeline = VK_NULL_HANDLE;
      // Created during guest output painting (after awaiting the last guest
      // output paint if outdated and needs to be recreated), when needed, for
      // the up-to-date render pass that draws to the swapchain with the actual
      // image format.
      VkPipeline swapchain_pipeline = VK_NULL_HANDLE;
      VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    };

    enum GuestOutputDescriptorSet : uint32_t {
      kGuestOutputDescriptorSetGuestOutput0Sampled,

      kGuestOutputDescriptorSetIntermediate0Sampled =
          kGuestOutputDescriptorSetGuestOutput0Sampled + kGuestOutputMailboxSize,

      kGuestOutputDescriptorSetCount =
          kGuestOutputDescriptorSetIntermediate0Sampled + kMaxGuestOutputPaintEffects - 1,
    };

    struct UISetupCommandBuffer {
      UISetupCommandBuffer(VkCommandPool command_pool, VkCommandBuffer command_buffer,
                           uint64_t last_usage_submission_index = 0)
          : command_pool(command_pool),
            command_buffer(command_buffer),
            last_usage_submission_index(last_usage_submission_index) {}

      VkCommandPool command_pool;
      VkCommandBuffer command_buffer;
      uint64_t last_usage_submission_index;
    };

    struct SwapchainFramebuffer {
      SwapchainFramebuffer(VkImageView image_view, VkFramebuffer framebuffer,
                           VkSemaphore present_semaphore)
          : image_view(image_view),
            framebuffer(framebuffer),
            present_semaphore(present_semaphore) {}

      VkImageView image_view;
      VkFramebuffer framebuffer;
      // Signaled by the paint and awaited by the image's present: one per
      // image, since a present may still be waiting for it when the next
      // paint submission comes around (VUID-vkQueueSubmit-pSignalSemaphores-00067).
      VkSemaphore present_semaphore;
    };

    // present_hdr: the swapchain is scRGB, and everything above paints into an
    // SDR stand-in per swapchain image as it would into an SDR swapchain; one
    // last pass decodes its sRGB and scales it to the paper white.
    struct HdrImage {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      // Sampled by the last pass, which decodes the sRGB.
      VkImageView srgb_view = VK_NULL_HANDLE;
      VkImageView swapchain_view = VK_NULL_HANDLE;
      VkFramebuffer swapchain_framebuffer = VK_NULL_HANDLE;
      VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    };
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // HDR10 where the surface offers no float format with it (Android).
    static constexpr VkFormat kHdrPqFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    static constexpr VkFormat kHdrStandInFormat = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkFormat kHdrStandInSrgbFormat = VK_FORMAT_B8G8R8A8_SRGB;

    explicit PaintContext(const VulkanDevice* const vulkan_device)
        : vulkan_device(vulkan_device), submission_tracker(vulkan_device) {}
    PaintContext(const PaintContext& paint_context) = delete;
    PaintContext& operator=(const PaintContext& paint_context) = delete;

    // The old swapchain, if passed, should be assumed to be retired after this
    // call (though it may fail before the vkCreateSwapchainKHR that will
    // technically retire it, so it will be in an undefined state), and needs to
    // be destroyed externally no matter what the result is.
    static VkSwapchainKHR CreateSwapchainForVulkanSurface(
        const VulkanDevice* vulkan_device, VkSurfaceKHR surface, uint32_t width, uint32_t height,
        VkSwapchainKHR old_swapchain, uint32_t& present_queue_family_out,
        VkFormat& image_format_out, VkExtent2D& image_extent_out, bool& is_fifo_out,
        bool& hdr_out, bool& ui_surface_unusable_out);

    // Destroys the swapchain and its derivatives, nulls `swapchain` and returns
    // the original swapchain object, if it existed, for use as oldSwapchain if
    // needed and for destruction.
    VkSwapchainKHR PrepareForSwapchainRetirement();
    // May be called from the destructor of the presenter.
    void DestroySwapchainAndVulkanSurface();

    // Connection-indepedent.

    const VulkanDevice* vulkan_device;

    std::array<std::unique_ptr<PaintContext::Submission>, kSubmissionCount> submissions;
    VulkanSubmissionTracker submission_tracker;

    std::array<GuestOutputPaintPipeline, size_t(GuestOutputPaintEffect::kCount)>
        guest_output_paint_pipelines;

    VkDescriptorPool guest_output_descriptor_pool = VK_NULL_HANDLE;
    // Descriptors are updated while painting if they're out of date.
    VkDescriptorSet guest_output_descriptor_sets[kGuestOutputDescriptorSetCount];

    // Refreshed and cleaned up during guest output painting. The first is the
    // paint submission index in which the guest output image (and its
    // descriptors) was last used, the second is the reference to the image,
    // which may be null. The indices are not mailbox indices here, rather, if
    // the reference is not in this array yet, the most outdated reference, if
    // needed, is replaced with the new one, awaiting the usage completion of
    // the last paint usage.
    std::array<std::pair<uint64_t, std::shared_ptr<GuestOutputImage>>, kGuestOutputMailboxSize>
        guest_output_image_paint_refs;
    // The latest submission index at which any guest output image was drawn.
    uint64_t guest_output_image_paint_last_submission = 0;

    // Current intermediate images for guest output painting, refreshed when
    // painting guest output.
    std::array<std::unique_ptr<GuestOutputImage>, kMaxGuestOutputPaintEffects - 1>
        guest_output_intermediate_images;
    // Created and destroyed alongside the images. UNORM only.
    std::array<VkFramebuffer, kMaxGuestOutputPaintEffects - 1>
        guest_output_intermediate_framebuffers = {};
    uint64_t guest_output_intermediate_image_last_submission = 0;

    // Command buffers optionally executed before the draw command buffer,
    // outside the painting render pass.
    std::vector<UISetupCommandBuffer> ui_setup_command_buffers;
    size_t ui_setup_command_buffer_current_index = SIZE_MAX;

    // Connection-specific.

    // May be reused between connections if the format stays the same.
    VkRenderPass swapchain_render_pass = VK_NULL_HANDLE;
    VkFormat swapchain_render_pass_format = VK_FORMAT_UNDEFINED;
    bool swapchain_render_pass_clear_load_op = false;
    // Whether swapchain_render_pass ends in an HDR stand-in to be sampled.
    bool swapchain_render_pass_hdr = false;
    // The last pass into the HDR swapchain, and its pipeline, for a swapchain
    // of hdr_render_pass_format.
    VkRenderPass hdr_render_pass = VK_NULL_HANDLE;
    VkFormat hdr_render_pass_format = VK_FORMAT_UNDEFINED;
    VkPipeline hdr_pipeline = VK_NULL_HANDLE;
    // Replaces the alpha of the painted guest output with the guest output's
    // HUD coverage, in swapchain_render_pass (always kHdrStandInFormat).
    VkPipeline hdr_ui_mask_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool hdr_descriptor_pool = VK_NULL_HANDLE;

    VkSurfaceKHR vulkan_surface = VK_NULL_HANDLE;
    // The ANativeWindow of vulkan_surface on Android, for its data space.
    void* android_window = nullptr;
#if REX_PLATFORM_MAC
    // Borrowed from the connected surface, under the painting lock.
    const void* mac_metal_layer = nullptr;
#endif
    uint32_t present_queue_family = UINT32_MAX;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    // vulkan_present_wait: the last present ID given on `swapchain`, 0 for
    // none yet.
    uint64_t swapchain_last_present_id = 0;
    VkExtent2D swapchain_extent = {};
    bool swapchain_is_fifo = false;
    std::vector<VkImage> swapchain_images;
    std::vector<SwapchainFramebuffer> swapchain_framebuffers;
    bool swapchain_hdr = false;
    // Per swapchain image when swapchain_hdr.
    std::vector<HdrImage> hdr_images;
  };

  explicit VulkanPresenter(HostGpuLossCallback host_gpu_loss_callback,
                           const VulkanDevice* vulkan_device, const UISamplers* ui_samplers)
      : Presenter(host_gpu_loss_callback),
        vulkan_device_(vulkan_device),
        ui_samplers_(ui_samplers),
        guest_output_image_refresher_submission_tracker_(vulkan_device),
        ui_submission_tracker_(vulkan_device),
        paint_context_(vulkan_device) {
    assert_not_null(vulkan_device);
    assert_not_null(ui_samplers);
  }

  bool InitializeSurfaceIndependent();

  // fragment_shader replaces the effect's own, with the effect's layout
  // unless layout is given.
  [[nodiscard]] VkPipeline CreateGuestOutputPaintPipeline(
      GuestOutputPaintEffect effect, VkRenderPass render_pass,
      VkShaderModule fragment_shader = VK_NULL_HANDLE,
      VkColorComponentFlags color_write_mask = VK_COLOR_COMPONENT_R_BIT |
                                               VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT |
                                               VK_COLOR_COMPONENT_A_BIT,
      VkPipelineLayout layout = VK_NULL_HANDLE);
  bool CreateHdrSwapchainImages(VkFormat swapchain_format);

  // present_upscaler_profile: the timestamp slot of this upscale, or
  // UINT32_MAX.
  uint32_t BeginTemporalUpscaleTiming(VkCommandBuffer command_buffer,
                                      const VulkanUpscaler& upscaler, const UpscalerFrame& frame);
  // The temporal provider for the selected effect, or null.
  VulkanUpscaler* GetTemporalUpscaler(const GuestOutputPaintConfig& config);
  // Records the temporal upscale of the flow's temporal slot from source into
  // output (UP-1.1: once per new guest frame, its history reset only when it
  // must be). False if nothing usable was recorded; the slot then falls back
  // to FSR 1 EASU.
  bool DispatchTemporalUpscaler(VkCommandBuffer command_buffer, VulkanUpscaler& upscaler,
                                VkImage source_image, VkImageView source_view,
                                uint32_t source_width, uint32_t source_height,
                                VkImage output_image, VkImageView output_view,
                                uint32_t output_width, uint32_t output_height,
                                uint64_t guest_output_refresh_serial,
                                const GuestOutputPaintConfig& config,
                                const GuestOutputImage* camera_source = nullptr,
                                const UpscalerCamera* camera = nullptr);

  const VulkanDevice* vulkan_device_;
  const UISamplers* ui_samplers_;

  // Static objects for guest output presentation, used only when painting the
  // main target (can be destroyed only after awaiting main target usage
  // completion).
  VkDescriptorSetLayout guest_output_paint_image_descriptor_set_layout_ = VK_NULL_HANDLE;
  std::array<VkPipelineLayout, kGuestOutputPaintPipelineLayoutCount>
      guest_output_paint_pipeline_layouts_ = {};
  VkShaderModule guest_output_paint_vs_ = VK_NULL_HANDLE;
  VkShaderModule hdr_output_fs_ = VK_NULL_HANDLE;
  VkShaderModule hdr_ui_mask_fs_ = VK_NULL_HANDLE;
  std::array<VkShaderModule, size_t(GuestOutputPaintEffect::kCount)> guest_output_paint_fs_ = {};
  // Not compatible with the swapchain render pass even if the format is the
  // same due to different dependencies (this is shader read > color
  // attachment > shader read).
  VkRenderPass guest_output_intermediate_render_pass_ = VK_NULL_HANDLE;

  // Value monotonically increased every time a new guest output image is
  // initialized, for recreation of dependent objects such as framebuffers in
  // the refreshers - saving and comparing the handle in the refresher is not
  // enough as Create > Destroy > Create may result in the same handle for
  // actually different objects without the refresher being aware of the
  // destruction.
  uint64_t guest_output_image_next_version_ = 0;
  std::array<GuestOutputImageInstance, kGuestOutputMailboxSize> guest_output_images_;
  VulkanSubmissionTracker guest_output_image_refresher_submission_tracker_;

  // UI submission tracker with the submission index that can be given to UI
  // drawers (accessible from the UI thread only, at any time).
  VulkanSubmissionTracker ui_submission_tracker_;

  // Accessible only by painting and by surface connection lifetime management
  // (ConnectOrReconnectPaintingToSurfaceFromUIThread,
  // DisconnectPaintingFromSurfaceFromUIThreadImpl) by the thread doing it, as
  // well as by presenter initialization and shutdown.
  PaintContext paint_context_;

  // Advanced by every guest output refresh (refresher thread; the consumer
  // reads the copy in GuestOutputImageInstance).
  uint64_t guest_output_refresh_serial_ = 0;

  // Temporal upscaler providers (rex/ui/vulkan/upscaler.h) and the state of
  // the last temporal dispatch, used by painting only.
  std::vector<std::unique_ptr<VulkanUpscaler>> upscalers_;
  struct TemporalUpscaleState {
    VulkanUpscaler* upscaler = nullptr;
    VkImage output_image = VK_NULL_HANDLE;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint64_t refresh_serial = 0;
    uint64_t last_dispatch_us = 0;
    bool output_valid = false;
    uint64_t dispatch_count = 0;
    uint64_t reset_count = 0;
    uint64_t repeat_count = 0;
  };
  TemporalUpscaleState temporal_upscale_;
  // UP-1.2: whether the last paint fed a temporal upscaler the guest output
  // directly, so the refresher should write the HUD-less copy.
  std::atomic<bool> hudless_guest_output_wanted_ = false;
  // UP-1.3: whether it would use the depth and camera motion.
  std::atomic<bool> guest_output_camera_wanted_ = false;
  // UP-1.5: the jitter sequence length it wants, 0 for none.
  std::atomic<uint32_t> guest_output_jitter_phases_ = 0;
  // UP-1.2: draws the HUD over the upscale of the HUD-less copy
  // (upscale_hud_composite.cs.glsl), created on first use.
  bool EnsureUpscaleHudComposite();
  bool upscale_hud_composite_failed_ = false;
  VkDescriptorSetLayout upscale_hud_composite_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout upscale_hud_composite_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline upscale_hud_composite_pipeline_ = VK_NULL_HANDLE;
  // present_upscaler_view (upscale_debug_view.cs.glsl), same layout.
  VkPipeline upscale_debug_view_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool upscale_hud_composite_descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, PaintContext::kSubmissionCount> upscale_hud_composite_sets_ = {};
  VkQueryPool temporal_upscale_query_pool_ = VK_NULL_HANDLE;
  std::array<uint64_t, 8> temporal_upscale_query_submissions_ = {};
  double temporal_upscale_gpu_ns_ = 0.0;
  uint32_t temporal_upscale_gpu_samples_ = 0;

  // CapturePresentedOutput (present_capture_readable): every paint copies its
  // finished image here, and a capture reads the latest copy once the queue
  // is idle (the route is paused then, so no paint follows to wait for).
  std::mutex presented_capture_mutex_;
  VkBuffer presented_capture_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory presented_capture_memory_ = VK_NULL_HANDLE;
  VkExtent2D presented_capture_buffer_extent_ = {};
  VkExtent2D presented_capture_extent_ = {};
  VkFormat presented_capture_format_ = VK_FORMAT_UNDEFINED;
  uint64_t presented_capture_submission_ = 0;
  bool presented_capture_written_ = false;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
