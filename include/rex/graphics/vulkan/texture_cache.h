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

#include <array>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/hash.h>
#include <rex/ui/vulkan/mem_alloc.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanTextureCache final : public TextureCache {
 public:
  // Sampler parameters that can be directly converted to a host sampler or used
  // for checking whether samplers bindings are up to date.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
      // Maximum mip level is in the texture resource itself, but mip_base_map
      // can be used to limit fetching to mip_min_level.
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    struct Hasher {
      size_t operator()(const SamplerParameters& parameters) const {
        return std::hash<uint32_t>{}(parameters.value);
      }
    };
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };

  // Transient descriptor set layouts must be initialized in the command
  // processor.
  static std::unique_ptr<VulkanTextureCache> Create(
      const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
      uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
      VulkanCommandProcessor& command_processor,
      VkPipelineStageFlags guest_shader_pipeline_stages) {
    std::unique_ptr<VulkanTextureCache> texture_cache(new VulkanTextureCache(
        register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y,
        command_processor, guest_shader_pipeline_stages));
    if (!texture_cache->Initialize()) {
      return nullptr;
    }
    return std::move(texture_cache);
  }

  ~VulkanTextureCache();

  void BeginSubmission(uint64_t new_submission_index) override;
  void BeginFrame() override;
  void EndFrame();

  // Must be called within a frame - creates and untiles textures needed by
  // shaders, and enqueues transitioning them into the sampled usage. This may
  // bind compute pipelines (notifying the command processor about that), and
  // also since it may insert deferred barriers, before flushing the barriers
  // preceding host GPU work.
  void RequestTextures(uint32_t used_texture_mask) override;

  // Diagnostics: writes level 0 of the textures based at this address, as
  // the GPU has them now (waiting for it), to <path>-<n>.bin.
  void DebugDumpTextures(uint32_t base_address, const std::string& path);
  VkImageView GetActiveBindingOrNullImageView(uint32_t fetch_constant_index,
                                              xenos::FetchOpDimension dimension, bool is_signed);

  // PD-4 bindless textures: every sampled image view and sampler has a slot
  // in one long-lived descriptor set for its whole life (it is written once,
  // and freed only when the view or sampler is destroyed, after its last
  // submission completed). Slot 0 of each array is the null view or a default
  // sampler.
  bool bindless() const { return bindless_.set != VK_NULL_HANDLE; }
  // Bumped whenever texture bindings were updated (their views and slots).
  uint64_t bindings_generation() const { return bindings_generation_; }
  VkDescriptorSetLayout bindless_descriptor_set_layout() const { return bindless_.layout; }
  VkDescriptorSet bindless_descriptor_set() const { return bindless_.set; }
  // The binding's cached slot inline; 3D as 2D, null and new views out of line.
  uint32_t GetActiveBindingBindlessIndex(uint32_t fetch_constant_index,
                                         xenos::FetchOpDimension dimension, bool is_signed) {
    const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
    if (binding && AreDimensionsCompatible(dimension, binding->key.dimension) &&
        !(binding->key.dimension == xenos::DataDimension::k3D &&
          (dimension == xenos::FetchOpDimension::k1D ||
           dimension == xenos::FetchOpDimension::k2D))) {
      const VulkanTextureBinding& vulkan_binding = vulkan_texture_bindings_[fetch_constant_index];
      const uint32_t slot =
          is_signed ? vulkan_binding.bindless_slot_signed : vulkan_binding.bindless_slot_unsigned;
      if (slot != UINT32_MAX) {
        return slot;
      }
    }
    return GetActiveBindingBindlessIndexSlow(fetch_constant_index, dimension, is_signed);
  }
  uint32_t GetActiveBindingBindlessIndexSlow(uint32_t fetch_constant_index,
                                             xenos::FetchOpDimension dimension, bool is_signed);

  SamplerParameters GetSamplerParameters(const VulkanShader::SamplerBinding& binding) const;

  // Must be called for every used sampler at least once in a single submission,
  // and a submission must be open for this to be callable.
  // Returns:
  // - The sampler, if obtained successfully - and increases its last usage
  //   submission index - and has_overflown_out = false.
  // - VK_NULL_HANDLE and has_overflown_out = true if there's a total sampler
  //   count overflow in a submission that potentially hasn't completed yet.
  // - VK_NULL_HANDLE and has_overflown_out = false in case of a general failure
  //   to create a sampler.
  VkSampler UseSampler(SamplerParameters parameters, bool& has_overflown_out);
  // Returns the submission index to await (may be the current submission in
  // case of an overflow within a single submission - in this case, it must be
  // ended, and a new one must be started) in case of sampler count overflow, so
  // samplers may be freed, and UseSamplers may take their slots.
  uint64_t GetSubmissionToAwaitOnSamplerOverflow(uint32_t overflowed_sampler_count) const;
  // Destroys every sampler, so UseSampler creates them again with the current
  // texture_mip_lod_bias. No submission may still use them.
  void ClearSamplers();
  // With bindless, also the sampler's slot (0 when there is no sampler).
  uint32_t GetSamplerBindlessIndex(VkSampler sampler) const;

  // Returns the 2D view of the front buffer texture (for fragment shader
  // reading - the barrier will be pushed in the command processor if needed),
  // or VK_NULL_HANDLE in case of failure. May call LoadTextureData.
  // If swap_source_needs_rb_swap_out is not nullptr, writes whether the final
  // guest-to-host swizzle requires swapping red and blue (R <- B, B <- R) with
  // green preserved, which is needed by the presentation fallback path on
  // devices without imageViewFormatSwizzle.
  VkImageView RequestSwapTexture(uint32_t& width_scaled_out, uint32_t& height_scaled_out,
                                 xenos::TextureFormat& format_out,
                                 uint32_t* width_unscaled_out = nullptr,
                                 uint32_t* height_unscaled_out = nullptr,
                                 bool* swap_source_needs_rb_swap_out = nullptr);

  bool GetScaledResolveRange(uint32_t start_unscaled, uint32_t length_unscaled,
                             uint32_t length_scaled_alignment_log2, uint64_t& start_scaled_out,
                             uint64_t& length_scaled_out) const;
  bool CommitScaledResolveRange(uint32_t start_unscaled, uint32_t length_unscaled,
                                uint32_t length_scaled_alignment_log2 = 0) {
    return EnsureScaledResolveMemoryCommitted(start_unscaled, length_unscaled,
                                              length_scaled_alignment_log2);
  }
  VkBuffer scaled_resolve_buffer() const { return scaled_resolve_buffer_; }
  void UseScaledResolveBufferForRead();
  void UseScaledResolveBufferForWrite(uint64_t written_start_scaled,
                                      uint64_t written_length_scaled);

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsHostGammaSupported(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;

  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  bool EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled, uint32_t length_unscaled,
                                          uint32_t length_scaled_alignment_log2 = 0) override;

  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;
  bool LoadTextureDataFromResidentMemoryUntimed(Texture& texture, bool load_base, bool load_mips);

  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;

 private:
  enum LoadDescriptorSetIndex {
    kLoadDescriptorSetIndexDestination,
    kLoadDescriptorSetIndexSource,
    kLoadDescriptorSetCount,
  };

  struct HostFormat {
    LoadShaderIndex load_shader;
    // Do NOT add integer formats to this - they are not filterable, can only be
    // read with ImageFetch, not ImageSample! If any game is seen using
    // num_format 1 for fixed-point formats (for floating-point, it's normally
    // set to 1 though), add a constant buffer containing multipliers for the
    // textures and multiplication to the tfetch implementation.
    VkFormat format;
    // Whether the format is block-compressed on the host (the host block size
    // matches the guest format block size in this case), and isn't decompressed
    // on load.
    bool block_compressed;

    // Set up dynamically based on what's supported by the device.
    bool linear_filterable;
  };

  struct HostFormatPair {
    HostFormat format_unsigned;
    HostFormat format_signed;
    // Mapping of Xenos swizzle components to Vulkan format components.
    uint32_t swizzle;
    // Whether the unsigned and the signed formats are compatible for one image
    // and the same image data (on a portability subset device, this should also
    // take imageViewFormatReinterpretation into account).
    bool unsigned_signed_compatible;
  };

  class VulkanTexture final : public Texture {
   public:
    enum class Usage {
      kUndefined,
      kTransferDestination,
      kGuestShaderSampled,
      kSwapSampled,
      // Written by the buffer-to-image copy compute shader.
      kComputeWrite,
    };

    // Takes ownership of the image and its memory.
    explicit VulkanTexture(VulkanTextureCache& texture_cache, const TextureKey& key, VkImage image,
                           VmaAllocation allocation, bool track_usage = true);
    ~VulkanTexture();

    VkImage image() const { return image_; }

    // Doesn't transition (the caller must insert the barrier).
    Usage SetUsage(Usage new_usage) {
      Usage old_usage = usage_;
      usage_ = new_usage;
      return old_usage;
    }

    // srgb: the unsigned view in the sRGB version of the format
    // (texture_gamma_host_srgb, IsHostGammaSupported).
    VkImageView GetView(bool is_signed, uint32_t host_swizzle, bool is_array = true,
                        bool srgb = false);
    VkImageView GetViewUncached(bool is_signed, uint32_t host_swizzle, bool is_array,
                                bool srgb = false);
    VkImageView GetOrCreate3DAs2DImageView(bool is_signed, uint32_t host_swizzle);

    // Loads by compute (vulkan_texture_load_compute_copy): the image was
    // created storage-capable with a raw-bits view format of copy_words()
    // 32-bit words per texel (0: it was not), and the 2D array view of level 0
    // in that format.
    uint32_t copy_words() const { return copy_words_; }
    void set_copy_words(uint32_t words) { copy_words_ = words; }
    VkImageView GetCopyView();
    // PD-4: the bindless slot of a view GetView returned, remembered with the
    // last view of the signedness.
    uint32_t GetBindlessSlot(bool is_signed, VkImageView view, uint32_t array);
    // The same format's 2D view of level 0 and layer 0, for the FH1 resolve
    // writing the texture directly (a storage image declared 2D, which a 2D
    // array view does not match).
    VkImageView GetCopyView2D();

   private:
    union ViewKey {
      uint32_t key;
      struct {
        uint32_t is_signed_separate_view : 1;
        uint32_t host_swizzle : 12;
        uint32_t is_array : 1;
        uint32_t is_srgb : 1;
      };

      ViewKey() : key(0) { static_assert_size(*this, sizeof(key)); }

      struct Hasher {
        size_t operator()(const ViewKey& key) const {
          return std::hash<decltype(key.key)>{}(key.key);
        }
      };
      bool operator==(const ViewKey& other_key) const { return key == other_key.key; }
      bool operator!=(const ViewKey& other_key) const { return !(*this == other_key); }
    };

    static constexpr VkComponentSwizzle GetComponentSwizzle(uint32_t texture_swizzle,
                                                            uint32_t component_index) {
      xenos::XE_GPU_TEXTURE_SWIZZLE texture_component_swizzle =
          xenos::XE_GPU_TEXTURE_SWIZZLE((texture_swizzle >> (3 * component_index)) & 0b111);
      if (texture_component_swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE(component_index)) {
        // The portability subset requires all swizzles to be IDENTITY, return
        // IDENTITY specifically, not R, G, B, A.
        return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
      switch (texture_component_swizzle) {
        case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
          return VK_COMPONENT_SWIZZLE_R;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
          return VK_COMPONENT_SWIZZLE_G;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
          return VK_COMPONENT_SWIZZLE_B;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
          return VK_COMPONENT_SWIZZLE_A;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
          return VK_COMPONENT_SWIZZLE_ZERO;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
          return VK_COMPONENT_SWIZZLE_ONE;
        default:
          // An invalid value.
          return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
    }

    VkImage image_;
    VmaAllocation allocation_;
    uint32_t copy_words_ = 0;
    VkImageView copy_view_ = VK_NULL_HANDLE;
    VkImageView copy_view_2d_ = VK_NULL_HANDLE;

    Usage usage_ = Usage::kUndefined;

    std::unordered_map<ViewKey, VkImageView, ViewKey::Hasher> views_;
    // The last view returned per signedness, by its GetView arguments: a
    // texture is nearly always bound with one swizzle, and views live as long
    // as the texture.
    struct LastView {
      uint32_t arguments = UINT32_MAX;
      VkImageView view = VK_NULL_HANDLE;
      // PD-4: the view's bindless slot once known.
      uint32_t bindless_slot = UINT32_MAX;
    };
    LastView last_views_[2];
    std::unique_ptr<VulkanTexture> texture_3d_as_2d_;
    VkImageView image_view_3d_as_2d_unsigned_ = VK_NULL_HANDLE;
    VkImageView image_view_3d_as_2d_signed_ = VK_NULL_HANDLE;
  };

  struct VulkanTextureBinding {
    VkImageView image_view_unsigned;
    VkImageView image_view_signed;
    // Bindless slots of the views (UINT32_MAX when not known yet).
    uint32_t bindless_slot_unsigned;
    uint32_t bindless_slot_signed;

    VulkanTextureBinding() { Reset(); }

    void Reset() {
      image_view_unsigned = VK_NULL_HANDLE;
      image_view_signed = VK_NULL_HANDLE;
      bindless_slot_unsigned = UINT32_MAX;
      bindless_slot_signed = UINT32_MAX;
    }
  };

  struct Sampler {
    VkSampler sampler;
    uint32_t bindless_slot = 0;
    bool uses_custom_border_color;
    uint64_t last_usage_submission;
    std::pair<const SamplerParameters, Sampler>* used_previous;
    std::pair<const SamplerParameters, Sampler>* used_next;
  };

  static constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                                xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  explicit VulkanTextureCache(const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
                              uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                              VulkanCommandProcessor& command_processor,
                              VkPipelineStageFlags guest_shader_pipeline_stages);

  bool Initialize();
  bool InitializeScaledResolveBuffer();
  void ShutdownScaledResolveBuffer();

  const HostFormatPair& GetHostFormatPair(TextureKey key) const;

  // For a resolve writing straight into the texture (DR-2.2): its raw-bits
  // view of single 32-bit words, with the image ready for compute writes and
  // its contents kept, or VK_NULL_HANDLE.
 public:
  VkImageView PrepareDirectResolveWrite(Texture& texture);

 private:
  void GetTextureUsageMasks(VulkanTexture::Usage usage, VkPipelineStageFlags& stage_mask,
                            VkAccessFlags& access_mask, VkImageLayout& layout);
  bool EnsureScaledResolveBufferAllocated(uint64_t start_scaled, uint64_t length_scaled);
  void GetScaledResolveUsageMasks(VkPipelineStageFlags& stage_mask_out,
                                  VkAccessFlags& access_mask_out, bool write) const;

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // Using the Vulkan Memory Allocator because texture count in games is
  // naturally pretty much unbounded, while Vulkan implementations, especially
  // on Windows versions before 10, may have an allocation count limit as low as
  // 4096.
  VmaAllocator vma_allocator_ = VK_NULL_HANDLE;

  static const HostFormatPair kBestHostFormats[64];
  static const HostFormatPair kHostFormatGBGRUnaligned;
  static const HostFormatPair kHostFormatBGRGUnaligned;
  static const HostFormatPair kHostFormatDXT1Unaligned;
  static const HostFormatPair kHostFormatDXT2_3Unaligned;
  static const HostFormatPair kHostFormatDXT4_5Unaligned;
  static const HostFormatPair kHostFormatDXNUnaligned;
  static const HostFormatPair kHostFormatDXT5AUnaligned;
  HostFormatPair host_formats_[64];

  VkPipelineLayout load_pipeline_layout_ = VK_NULL_HANDLE;
  // Buffer-to-image copies by compute (vulkan_texture_load_compute_copy):
  // set 0 the scratch storage buffer, set 1 the texture's raw-bits storage
  // view; pipelines by 32-bit words per texel (1, 2, 4).
  VkPipelineLayout copy_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline copy_pipelines_[3] = {};
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_{};
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_scaled_{};

  // If both images can be placed in the same allocation, it's one allocation,
  // otherwise it's two separate.
  std::array<VkDeviceMemory, 2> null_images_memory_{};
  VkImage null_image_2d_array_cube_ = VK_NULL_HANDLE;
  VkImage null_image_3d_ = VK_NULL_HANDLE;
  VkImageView null_image_view_2d_array_ = VK_NULL_HANDLE;
  VkImageView null_image_view_cube_ = VK_NULL_HANDLE;
  VkImageView null_image_view_3d_ = VK_NULL_HANDLE;
  bool null_images_cleared_ = false;

  std::array<VulkanTextureBinding, xenos::kTextureFetchConstantCount> vulkan_texture_bindings_;

  // PD-4 bindless set: arrays by SpirvShaderTranslator::BindlessBinding.
  struct Bindless {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkSampler default_sampler = VK_NULL_HANDLE;
    uint32_t capacity[4] = {};
    uint32_t next[4] = {};
    std::vector<uint32_t> free[4];
    uint64_t overflows = 0;
  } bindless_;
  uint64_t bindings_generation_ = 0;
  // Views given slots outside the per-binding cache (3D as 2D, null views):
  // view to (array, slot).
  std::unordered_map<VkImageView, std::pair<uint32_t, uint32_t>> bindless_view_slots_;
  std::unordered_map<VkSampler, uint32_t> bindless_sampler_slots_;
  bool InitializeBindless();
  void ShutdownBindless();
  uint32_t AllocateBindlessSlot(uint32_t array);
  void WriteBindlessImage(uint32_t array, uint32_t slot, VkImageView view);
  void WriteBindlessSampler(uint32_t slot, VkSampler sampler);
  // The view's slot, allocated and written on first use.
  uint32_t BindlessViewSlot(VkImageView view, uint32_t array);
 public:
  // Called by VulkanTexture for each view it destroys.
  void ReleaseBindlessView(VkImageView view);
 private:

  // Unsupported texture formats used during this frame (for research and
  // testing).
  enum : uint8_t {
    kUnsupportedResourceBit = 1,
    kUnsupportedUnormBit = kUnsupportedResourceBit << 1,
    kUnsupportedSnormBit = kUnsupportedUnormBit << 1,
  };
  uint8_t unsupported_format_features_used_[64] = {};

  uint32_t sampler_max_count_;

  xenos::AnisoFilter max_anisotropy_;

  std::unordered_map<SamplerParameters, Sampler, SamplerParameters::Hasher> samplers_;
  std::pair<const SamplerParameters, Sampler>* sampler_used_first_ = nullptr;
  std::pair<const SamplerParameters, Sampler>* sampler_used_last_ = nullptr;
  uint32_t custom_border_color_sampler_count_ = 0;

  VkBuffer scaled_resolve_buffer_ = VK_NULL_HANDLE;
  uint64_t scaled_resolve_buffer_size_ = 0;
  bool scaled_resolve_buffer_sparse_ = false;
  uint32_t scaled_resolve_buffer_memory_type_ = UINT32_MAX;
  std::vector<VkDeviceMemory> scaled_resolve_buffer_memory_;
  uint32_t scaled_resolve_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> scaled_resolve_sparse_allocated_;
  bool scaled_resolve_last_usage_write_ = false;
  std::pair<uint64_t, uint64_t> scaled_resolve_last_written_range_{0, 0};
};

}  // namespace rex::graphics::vulkan
