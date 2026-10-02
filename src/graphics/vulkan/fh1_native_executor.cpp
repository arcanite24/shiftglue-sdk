#include <rex/graphics/vulkan/fh1_native_executor.h>

#include <algorithm>
#include <unordered_set>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/util.h>


REXCVAR_DEFINE_BOOL(fh1_scaled_msaa_single_sample, false, "GPU",
                    "Above 1x resolution scale, keep the guest's 2x and 4x MSAA surfaces as "
                    "single-sampled images (Vulkan): every guest sample of a pixel is its one "
                    "host sample. Much less GPU work at 3x and 4x; edges lose their MSAA")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(fh1_msaa_single_sample, false, "GPU",
                    "Keep the guest's 2x and 4x MSAA surfaces as single-sampled images at any "
                    "resolution scale (Vulkan): at 1x the game renders without MSAA - harder "
                    "edges - for much less GPU work, the cheaper choice for phones")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_native_stencil_export, true, "GPU",
                    "Transfer depth and stencil into a depth surface in one pass where the "
                    "device exports stencil from shaders (FH1 native executor, Vulkan), "
                    "instead of a pass per stencil bit")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_fold_clears, true, "GPU",
                    "Record EDRAM clears inside the next draw rendering on the surface (or as "
                    "its load op) instead of in a rendering of their own: on tiled GPUs each "
                    "rendering costs a pass over the tiles (FH1 native executor, Vulkan)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fh1_debug_rendering_split_draws, 0, "GPU",
                     "Diagnostics: end the draw rendering every N draws (0: never), to measure "
                     "the cost of a rendering on the GPU. Same image, more renderings")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_skip_clears, false, "GPU",
                    "Diagnostics: skip every EDRAM clear (wrong image), to bound what clears "
                    "cost")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#if REX_HAS_D3D12
REXCVAR_DECLARE(std::string, fh1_resolve_dump_dir);
REXCVAR_DECLARE(int32_t, fh1_resolve_dump_frame);
#else
REXCVAR_DEFINE_INT32(fh1_resolve_dump_frame, 0, "GPU",
                     "Diagnostics: with fh1_resolve_dump_dir, dump only the resolves of this "
                     "frame (swap count; 0 dumps every frame)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_resolve_dump_dir, "", "GPU",
                      "Diagnostics: write every resolve's output bytes to this directory, "
                      "waiting for the GPU after each")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#endif

namespace rex::graphics::vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/fh1_native_resolve_memory_color_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_resolve_memory_color_ms_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_resolve_memory_depth_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_resolve_memory_depth_ms_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_resolve_memory_uint_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_resolve_memory_uint_ms_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_dms_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_dms_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_dms_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_dms_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_dms_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_color_dms_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_dms_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_dms_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_dms_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_dms_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_dms_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_dms_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_dms_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_dms_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_dms_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_dms_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_dms_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_dms_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_dms_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_dms_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_dms_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_dms_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_dms_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_uint_dms_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_words_color_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_words_color_ms_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_words_depth_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_words_depth_ms_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_words_uint_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_words_uint_ms_cs.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_from_words_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_dms_from_words_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_from_words_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_stencil_dms_from_words_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_dms_from_color_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_dms_from_color_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_dms_from_depth_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_dms_from_depth_ms_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_dms_from_uint_ps.h"
#include "../shaders/vulkan_spirv/fh1_native_transfer_depth_stencil_dms_from_uint_ms_ps.h"
#include "../shaders/vulkan_spirv/fullscreen_cw_vs.h"
}  // namespace shaders

namespace {

// Binding indices of the SPIR-V (compile-fh1-native.sh: t<n> is 16 + n, u<n>
// is 32 + n).
constexpr uint32_t kBindingSource = 16;
constexpr uint32_t kBindingStencil = 17;
constexpr uint32_t kBindingMemory = 32;
constexpr uint32_t kComputeConstantCount = 8;
constexpr uint32_t kTransferConstantCount = 3;

struct SpirvShader {
  const uint32_t* code;
  size_t size;
};
#define FH1_SPIRV(name) SpirvShader{shaders::name, sizeof(shaders::name)}

// [dest kind: color, depth, stencil bit, uint][dest msaa]
// [source kind: color, depth, uint][source msaa]
const SpirvShader kTransferShaders[4][2][3][2] = {
    {{{FH1_SPIRV(fh1_native_transfer_color_from_color_ps),
       FH1_SPIRV(fh1_native_transfer_color_from_color_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_color_from_depth_ps),
       FH1_SPIRV(fh1_native_transfer_color_from_depth_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_color_from_uint_ps),
       FH1_SPIRV(fh1_native_transfer_color_from_uint_ms_ps)}},
     {{FH1_SPIRV(fh1_native_transfer_color_dms_from_color_ps),
       FH1_SPIRV(fh1_native_transfer_color_dms_from_color_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_color_dms_from_depth_ps),
       FH1_SPIRV(fh1_native_transfer_color_dms_from_depth_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_color_dms_from_uint_ps),
       FH1_SPIRV(fh1_native_transfer_color_dms_from_uint_ms_ps)}}},
    {{{FH1_SPIRV(fh1_native_transfer_depth_from_color_ps),
       FH1_SPIRV(fh1_native_transfer_depth_from_color_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_depth_from_depth_ps),
       FH1_SPIRV(fh1_native_transfer_depth_from_depth_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_depth_from_uint_ps),
       FH1_SPIRV(fh1_native_transfer_depth_from_uint_ms_ps)}},
     {{FH1_SPIRV(fh1_native_transfer_depth_dms_from_color_ps),
       FH1_SPIRV(fh1_native_transfer_depth_dms_from_color_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_depth_dms_from_depth_ps),
       FH1_SPIRV(fh1_native_transfer_depth_dms_from_depth_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_depth_dms_from_uint_ps),
       FH1_SPIRV(fh1_native_transfer_depth_dms_from_uint_ms_ps)}}},
    {{{FH1_SPIRV(fh1_native_transfer_stencil_from_color_ps),
       FH1_SPIRV(fh1_native_transfer_stencil_from_color_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_stencil_from_depth_ps),
       FH1_SPIRV(fh1_native_transfer_stencil_from_depth_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_stencil_from_uint_ps),
       FH1_SPIRV(fh1_native_transfer_stencil_from_uint_ms_ps)}},
     {{FH1_SPIRV(fh1_native_transfer_stencil_dms_from_color_ps),
       FH1_SPIRV(fh1_native_transfer_stencil_dms_from_color_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_stencil_dms_from_depth_ps),
       FH1_SPIRV(fh1_native_transfer_stencil_dms_from_depth_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_stencil_dms_from_uint_ps),
       FH1_SPIRV(fh1_native_transfer_stencil_dms_from_uint_ms_ps)}}},
    {{{FH1_SPIRV(fh1_native_transfer_uint_from_color_ps),
       FH1_SPIRV(fh1_native_transfer_uint_from_color_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_uint_from_depth_ps),
       FH1_SPIRV(fh1_native_transfer_uint_from_depth_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_uint_from_uint_ps),
       FH1_SPIRV(fh1_native_transfer_uint_from_uint_ms_ps)}},
     {{FH1_SPIRV(fh1_native_transfer_uint_dms_from_color_ps),
       FH1_SPIRV(fh1_native_transfer_uint_dms_from_color_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_uint_dms_from_depth_ps),
       FH1_SPIRV(fh1_native_transfer_uint_dms_from_depth_ms_ps)},
      {FH1_SPIRV(fh1_native_transfer_uint_dms_from_uint_ps),
       FH1_SPIRV(fh1_native_transfer_uint_dms_from_uint_ms_ps)}}},
};
// [depth, stencil bit][dest msaa]
const SpirvShader kFromWordsShaders[2][2] = {
    {FH1_SPIRV(fh1_native_transfer_depth_from_words_ps),
     FH1_SPIRV(fh1_native_transfer_depth_dms_from_words_ps)},
    {FH1_SPIRV(fh1_native_transfer_stencil_from_words_ps),
     FH1_SPIRV(fh1_native_transfer_stencil_dms_from_words_ps)},
};
// Depth and stencil at once with stencil export, straight from the source:
// [dest msaa][source kind][source msaa].
const SpirvShader kTransferDepthStencilShaders[2][3][2] = {
    {
     {FH1_SPIRV(fh1_native_transfer_depth_stencil_from_color_ps),
      FH1_SPIRV(fh1_native_transfer_depth_stencil_from_color_ms_ps)},
     {FH1_SPIRV(fh1_native_transfer_depth_stencil_from_depth_ps),
      FH1_SPIRV(fh1_native_transfer_depth_stencil_from_depth_ms_ps)},
     {FH1_SPIRV(fh1_native_transfer_depth_stencil_from_uint_ps),
      FH1_SPIRV(fh1_native_transfer_depth_stencil_from_uint_ms_ps)}},
    {
     {FH1_SPIRV(fh1_native_transfer_depth_stencil_dms_from_color_ps),
      FH1_SPIRV(fh1_native_transfer_depth_stencil_dms_from_color_ms_ps)},
     {FH1_SPIRV(fh1_native_transfer_depth_stencil_dms_from_depth_ps),
      FH1_SPIRV(fh1_native_transfer_depth_stencil_dms_from_depth_ms_ps)},
     {FH1_SPIRV(fh1_native_transfer_depth_stencil_dms_from_uint_ps),
      FH1_SPIRV(fh1_native_transfer_depth_stencil_dms_from_uint_ms_ps)}},
};
// [resolve, words][source kind][msaa]
const SpirvShader kComputeShaders[2][3][2] = {
    {{FH1_SPIRV(fh1_native_resolve_memory_color_cs),
      FH1_SPIRV(fh1_native_resolve_memory_color_ms_cs)},
     {FH1_SPIRV(fh1_native_resolve_memory_depth_cs),
      FH1_SPIRV(fh1_native_resolve_memory_depth_ms_cs)},
     {FH1_SPIRV(fh1_native_resolve_memory_uint_cs),
      FH1_SPIRV(fh1_native_resolve_memory_uint_ms_cs)}},
    {{FH1_SPIRV(fh1_native_transfer_words_color_cs),
      FH1_SPIRV(fh1_native_transfer_words_color_ms_cs)},
     {FH1_SPIRV(fh1_native_transfer_words_depth_cs),
      FH1_SPIRV(fh1_native_transfer_words_depth_ms_cs)},
     {FH1_SPIRV(fh1_native_transfer_words_uint_cs),
      FH1_SPIRV(fh1_native_transfer_words_uint_ms_cs)}},
};
#undef FH1_SPIRV

// Formats whose guest EDRAM words are the host channel bits (16-bit or 32-bit
// float channels besides 32_FLOAT): read and transferred through UINT views.
VkFormat ColorUintFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_UINT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

// Source variant of a surface: 0 color, 1 depth and stencil, 2 raw color bits.
uint32_t SourceKind(bool is_depth, uint32_t format) {
  if (is_depth) return 1;
  return ColorUintFormat(xenos::ColorRenderTargetFormat(format)) != VK_FORMAT_UNDEFINED ? 2 : 0;
}

uint32_t PackLayout(uint32_t base, uint32_t pitch, uint32_t msaa, bool is_64bpp, bool is_depth,
                    uint32_t format) {
  return (base & 0x7FF) | ((pitch & 0xFF) << 11) | ((msaa & 3) << 19) |
         (uint32_t(is_64bpp) << 21) | (uint32_t(is_depth) << 22) | ((format & 0xF) << 23);
}

// IEEE half to float, for 16-bit float clear values.
float HalfToFloat(uint16_t half) {
  const uint32_t sign = uint32_t(half & 0x8000) << 16;
  uint32_t exponent = (half >> 10) & 0x1F;
  uint32_t mantissa = half & 0x3FF;
  uint32_t bits;
  if (!exponent) {
    if (!mantissa) {
      bits = sign;
    } else {
      exponent = 113;
      while (!(mantissa & 0x400)) {
        mantissa <<= 1;
        --exponent;
      }
      bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
    }
  } else if (exponent == 0x1F) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

constexpr VkPipelineStageFlags kColorAttachmentStage =
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr VkAccessFlags kColorAttachmentAccess =
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
constexpr VkPipelineStageFlags kDepthAttachmentStage =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags kDepthAttachmentAccess =
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

}  // namespace

Fh1NativeExecutor::Fh1NativeExecutor(VulkanCommandProcessor& command_processor,
                                     const RegisterFile& register_file, memory::Memory& memory)
    : command_processor_(command_processor),
      register_file_(register_file),
      memory_(memory),
      draw_extent_estimator_(register_file, memory),
      depth_overwrite_(register_file, memory) {}

Fh1NativeExecutor::~Fh1NativeExecutor() { Shutdown(); }

bool Fh1NativeExecutor::Initialize(const Fh1VulkanExecutorConfig& config) {
  config_ = config;
  if (!config_.memory || !config_.textures || !config_.render_targets) return false;
  scale_ = config_.textures->draw_resolution_scale_x();
  single_sample_msaa_ = (scale_ >= 2 && REXCVAR_GET(fh1_scaled_msaa_single_sample)) ||
                        REXCVAR_GET(fh1_msaa_single_sample);
  stencil_export_ = REXCVAR_GET(fh1_native_stencil_export) &&
                    command_processor_.GetVulkanDevice()->extensions().ext_EXT_shader_stencil_export;
  if (single_sample_msaa_) {
    REXGPU_INFO("FH1 native executor (Vulkan): MSAA surfaces single-sampled at {}x", scale_);
  }
  if (config_.textures->draw_resolution_scale_y() != scale_ || scale_ > 4) {
    REXGPU_WARN("FH1 native executor (Vulkan): resolution scale {}x{} is not supported",
                config_.textures->draw_resolution_scale_x(),
                config_.textures->draw_resolution_scale_y());
    return false;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  if (!vulkan_device->properties().dynamicRendering) {
    REXGPU_WARN("FH1 native executor (Vulkan): dynamic rendering is required");
    return false;
  }
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (REXCVAR_GET(fh1_native_gpu_profile)) {
    VkQueryPoolCreateInfo query_pool_info = {};
    query_pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_pool_info.queryCount = kGpuProfileSlots * kGpuProfileQueries;
    if (dfn.vkCreateQueryPool(device, &query_pool_info, nullptr, &gpu_query_pool_) !=
        VK_SUCCESS) {
      gpu_query_pool_ = VK_NULL_HANDLE;
      REXGPU_WARN("FH1 native executor (Vulkan): no timestamp query pool for the GPU profile");
    }
  }

  auto create_set_layout = [&](const VkDescriptorSetLayoutBinding* bindings, uint32_t count,
                               VkDescriptorSetLayout& layout_out) {
    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = count;
    info.pBindings = bindings;
    return dfn.vkCreateDescriptorSetLayout(device, &info, nullptr, &layout_out) == VK_SUCCESS;
  };
  VkDescriptorSetLayoutBinding bindings[3] = {};
  bindings[0] = {kBindingSource, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[1] = {kBindingStencil, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[2] = {kBindingMemory, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  if (!create_set_layout(bindings, 3, compute_set_layout_)) return false;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  if (!create_set_layout(bindings, 2, transfer_set_layout_)) return false;
  bindings[0] = {kBindingSource, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  if (!create_set_layout(bindings, 1, words_set_layout_)) return false;

  auto create_pipeline_layout = [&](VkDescriptorSetLayout set_layout, VkShaderStageFlags stage,
                                    uint32_t constant_count, VkPipelineLayout& layout_out) {
    VkPushConstantRange range = {stage, 0, uint32_t(constant_count * sizeof(uint32_t))};
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &set_layout;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    return dfn.vkCreatePipelineLayout(device, &info, nullptr, &layout_out) == VK_SUCCESS;
  };
  if (!create_pipeline_layout(compute_set_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                              kComputeConstantCount, compute_pipeline_layout_) ||
      !create_pipeline_layout(transfer_set_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                              kTransferConstantCount, transfer_pipeline_layout_) ||
      !create_pipeline_layout(words_set_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                              kTransferConstantCount, words_pipeline_layout_)) {
    return false;
  }
  tiles_.Reset();
  initialized_ = true;
  REXGPU_INFO("FH1 native executor enabled: native (Vulkan)");
  return true;
}

void Fh1NativeExecutor::Shutdown() {
  if (gpu_query_pool_ != VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
    vulkan_device->functions().vkDestroyQueryPool(vulkan_device->device(), gpu_query_pool_,
                                                  nullptr);
    gpu_query_pool_ = VK_NULL_HANDLE;
  }
  if (initialized_) LogStats(frame_);
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  if (!vulkan_device) return;
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (initialized_ || !surfaces_.empty()) {
    command_processor_.Fh1AwaitAllQueueOperations();
  }
  initialized_ = false;
  for (auto& [packed, surface] : surfaces_) DestroySurface(surface);
  surfaces_.clear();
  for (auto& [key, pipeline] : transfer_pipelines_) dfn.vkDestroyPipeline(device, pipeline, nullptr);
  transfer_pipelines_.clear();
  for (auto& by_words : compute_pipelines_) {
    for (auto& by_kind : by_words) {
      for (VkPipeline& pipeline : by_kind) {
        if (pipeline) dfn.vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
      }
    }
  }
  for (auto& [code, module] : shader_modules_) dfn.vkDestroyShaderModule(device, module, nullptr);
  shader_modules_.clear();
  for (auto& pools : descriptor_pools_) {
    for (VkDescriptorPool pool : pools) dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    pools.clear();
  }
  auto destroy_buffer = [&](VkBuffer& buffer, VkDeviceMemory& memory) {
    if (buffer) dfn.vkDestroyBuffer(device, buffer, nullptr);
    if (memory) dfn.vkFreeMemory(device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
  };
  destroy_buffer(transfer_words_, transfer_words_memory_);
  transfer_words_size_ = 0;
  for (auto& [buffer, memory] : retired_buffers_) destroy_buffer(buffer, memory);
  retired_buffers_.clear();
  for (PendingReadback& readback : pending_readbacks_) {
    destroy_buffer(readback.buffer, readback.memory);
  }
  pending_readbacks_.clear();
  for (VkPipelineLayout* layout :
       {&compute_pipeline_layout_, &transfer_pipeline_layout_, &words_pipeline_layout_}) {
    if (*layout) dfn.vkDestroyPipelineLayout(device, *layout, nullptr);
    *layout = VK_NULL_HANDLE;
  }
  for (VkDescriptorSetLayout* layout :
       {&compute_set_layout_, &transfer_set_layout_, &words_set_layout_}) {
    if (*layout) dfn.vkDestroyDescriptorSetLayout(device, *layout, nullptr);
    *layout = VK_NULL_HANDLE;
  }
}

void Fh1NativeExecutor::DestroySurface(Surface& surface) {
  for (SurfaceFront& front : surface_front_) {
    front = SurfaceFront();
  }
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (VkImageView view : {surface.view, surface.uint_view, surface.sampled_view,
                           surface.stencil_view}) {
    if (view && view != surface.view && view != surface.uint_view) {
      dfn.vkDestroyImageView(device, view, nullptr);
    }
  }
  if (surface.view) dfn.vkDestroyImageView(device, surface.view, nullptr);
  if (surface.uint_view) dfn.vkDestroyImageView(device, surface.uint_view, nullptr);
  if (surface.image) dfn.vkDestroyImage(device, surface.image, nullptr);
  if (surface.memory) dfn.vkFreeMemory(device, surface.memory, nullptr);
  if (!surface.pending_clears.empty()) --pending_clear_surfaces_;
  surface = Surface();
}

Fh1NativeExecutor::SurfaceKey Fh1NativeExecutor::MakeColorKey(
    uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
    xenos::ColorRenderTargetFormat format) const {
  return SurfaceKey::Color(base, pitch_tiles, msaa, format, config_.gamma_as_unorm16);
}

Fh1NativeExecutor::SurfaceKey Fh1NativeExecutor::MakeDepthKey(
    uint32_t base, uint32_t pitch_tiles, uint32_t msaa, xenos::DepthRenderTargetFormat format) {
  return SurfaceKey::Depth(base, pitch_tiles, msaa, format);
}

uint32_t Fh1NativeExecutor::SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) const {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  return Fh1SurfaceHeight(pitch_tiles, msaa,
                          vulkan_device->properties().maxImageDimension2D, scale_);
}

Fh1NativeExecutor::Rect Fh1NativeExecutor::HostRect(const Rect& rect) const {
  return {rect.left * int32_t(scale_), rect.top * int32_t(scale_), rect.right * int32_t(scale_),
          rect.bottom * int32_t(scale_)};
}

uint32_t Fh1NativeExecutor::TransferFlags() const {
  return (config_.depth_float24_round ? 1u : 0u) | (config_.gamma_as_unorm16 ? 2u : 0u) |
         (config_.fixed16_truncated ? 0u : 4u) | ((scale_ - 1) << 12);
}

Fh1NativeExecutor::Surface* Fh1NativeExecutor::FindSurface(uint32_t packed_key) {
  SurfaceFront& front = surface_front_[(packed_key ^ (packed_key >> 7) ^ (packed_key >> 15)) &
                                       (std::size(surface_front_) - 1)];
  if (front.packed_key == packed_key) {
    return front.surface;
  }
  auto it = surfaces_.find(packed_key);
  if (it == surfaces_.end()) {
    return nullptr;
  }
  front.packed_key = packed_key;
  front.surface = &it->second;
  return front.surface;
}

VkImageAspectFlags Fh1NativeExecutor::AspectMask(const Surface& surface) const {
  return surface.key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                              : VK_IMAGE_ASPECT_COLOR_BIT;
}

void Fh1NativeExecutor::Transition(Surface& surface, VkImageLayout layout,
                                   VkPipelineStageFlags stage, VkAccessFlags access) {
  if (!surface.pending_clears.empty() &&
      layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
      layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
    // Anything but drawing sees the cleared contents.
    FlushPendingClears(surface);
  }
  constexpr VkAccessFlags kReadOnly = VK_ACCESS_SHADER_READ_BIT;
  if (surface.layout == layout && !(surface.access & ~kReadOnly) && !(access & ~kReadOnly)) {
    // Reads after reads in the same layout need no barrier.
    surface.stage |= stage;
    surface.access |= access;
    return;
  }
  VkImageSubresourceRange range = {AspectMask(surface), 0, 1, 0, 1};
  command_processor_.PushImageMemoryBarrier(surface.image, range, surface.stage, stage,
                                            surface.access, access, surface.layout, layout,
                                            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                            false);
  surface.layout = layout;
  surface.stage = stage;
  surface.access = access;
}

void Fh1NativeExecutor::TransitionForSampling(Surface& surface, VkPipelineStageFlags stage) {
  Transition(surface, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage, VK_ACCESS_SHADER_READ_BIT);
}

void Fh1NativeExecutor::TransitionForAttachment(Surface& surface, uint64_t open_rendering_id) {
  const VkImageLayout layout = surface.key.is_depth
                                   ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                   : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  if (surface.layout == layout && command_processor_.IsFh1RenderingOpen(open_rendering_id)) {
    attachment_barriers_skipped_ = true;
    return;
  }
  if (surface.key.is_depth) {
    Transition(surface, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kDepthAttachmentStage,
               kDepthAttachmentAccess);
  } else {
    Transition(surface, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorAttachmentStage,
               kColorAttachmentAccess);
  }
}

void Fh1NativeExecutor::BeginSurfaceRendering(Surface& surface, bool uint_view) {
  // Transfers and clears into the surface come after its pending clears.
  FlushPendingClears(surface);
  VkRenderingAttachmentInfo attachment = {};
  attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  attachment.imageView = uint_view && surface.uint_view ? surface.uint_view : surface.view;
  attachment.imageLayout = surface.layout;
  attachment.resolveMode = VK_RESOLVE_MODE_NONE;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea.extent = {surface.width * scale_, surface.height * scale_};
  info.layerCount = 1;
  if (surface.key.is_depth) {
    info.pDepthAttachment = &attachment;
    info.pStencilAttachment = &attachment;
  } else {
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &attachment;
  }
  // Transfer and clear scopes are keyed apart from draw scopes.
  const uint64_t id =
      (uint64_t(1) << 63) | (uint64_t(surface.key.Pack()) << 1) | uint64_t(uint_view);
  rendering_id_ = 0;
  command_processor_.SubmitBarriersAndBeginFh1Rendering(info, id);
}

Fh1NativeExecutor::Surface* Fh1NativeExecutor::GetOrCreateSurface(const SurfaceKey& key) {
  const uint32_t packed = key.Pack();
  if (Surface* existing = FindSurface(packed)) return existing;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  Surface surface;
  surface.key = key;
  surface.width = key.pitch_tiles * (xenos::kEdramTileWidthSamples >> msaa_x_log2);
  surface.height = SurfaceHeight(key.pitch_tiles, key.msaa);
  surface.samples = single_sample_msaa_ ? 1u
                    : key.msaa == uint32_t(xenos::MsaaSamples::k2X) && !config_.msaa_2x_supported
                        ? 4u
                        : 1u << key.msaa;
  if (key.is_depth) {
    surface.format =
        config_.render_targets->GetDepthVulkanFormat(xenos::DepthRenderTargetFormat(key.format));
  } else {
    const auto format = xenos::ColorRenderTargetFormat(key.format);
    surface.format = config_.render_targets->GetColorVulkanFormat(format);
    surface.uint_format = ColorUintFormat(format);
  }
  if (surface.format == VK_FORMAT_UNDEFINED || !surface.width || !surface.height) {
    Skip("surface_format");
    return nullptr;
  }
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.flags = surface.uint_format != VK_FORMAT_UNDEFINED
                         ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
                         : 0;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = surface.format;
  image_info.extent = {surface.width * scale_, surface.height * scale_, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VkSampleCountFlagBits(surface.samples);
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                     (key.is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(vulkan_device, image_info,
                                                        ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                        surface.image, surface.memory)) {
    Skip("surface_create");
    return nullptr;
  }
  auto create_view = [&](VkFormat format, VkImageAspectFlags aspect, VkImageView& view_out) {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = surface.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {aspect, 0, 1, 0, 1};
    return dfn.vkCreateImageView(device, &view_info, nullptr, &view_out) == VK_SUCCESS;
  };
  bool views_created;
  if (key.is_depth) {
    views_created =
        create_view(surface.format, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                    surface.view) &&
        create_view(surface.format, VK_IMAGE_ASPECT_DEPTH_BIT, surface.sampled_view) &&
        create_view(surface.format, VK_IMAGE_ASPECT_STENCIL_BIT, surface.stencil_view);
  } else {
    views_created = create_view(surface.format, VK_IMAGE_ASPECT_COLOR_BIT, surface.view);
    if (views_created && surface.uint_format != VK_FORMAT_UNDEFINED) {
      views_created = create_view(surface.uint_format, VK_IMAGE_ASPECT_COLOR_BIT, surface.uint_view);
    }
    surface.sampled_view = surface.uint_view ? surface.uint_view : surface.view;
  }
  if (!views_created) {
    DestroySurface(surface);
    Skip("surface_view_create");
    return nullptr;
  }
  Surface& stored = surfaces_.emplace(packed, surface).first->second;
  // Render targets must be initialized before use; zero is also what the
  // stencil tracking assumes for a new surface.
  TransitionForAttachment(stored);
  BeginSurfaceRendering(stored, false);
  VkClearAttachment clear = {};
  clear.aspectMask = AspectMask(stored);
  VkClearRect clear_rect = {};
  clear_rect.rect.extent = {stored.width * scale_, stored.height * scale_};
  clear_rect.layerCount = 1;
  command_processor_.deferred_command_buffer().CmdVkClearAttachments(1, &clear, 1, &clear_rect);
  Count("surface_created");
  return &stored;
}

void Fh1NativeExecutor::ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key,
                                   bool transfer) {
  const auto runs = tiles_.Claim(base, length, packed_key, transfer);
  if (runs.empty()) return;
  Surface* dest = FindSurface(packed_key);
  if (!dest) return;
  for (const TileRun& run : runs) TransferTiles(*dest, run);
}

void Fh1NativeExecutor::MarkTileStencil(uint32_t base, uint32_t length, bool nonzero) {
  tiles_.MarkStencil(base, length, nonzero);
}

uint32_t Fh1NativeExecutor::HostSampleMode(const Surface& surface) {
  const SurfaceKey& key = surface.key;
  if (key.msaa != uint32_t(xenos::MsaaSamples::k1X) && surface.samples == 1) {
    return 3u;  // Every guest sample in the one host sample.
  }
  return key.msaa == uint32_t(xenos::MsaaSamples::k2X) ? (surface.samples == 4 ? 2u : 1u) : 0u;
}

uint32_t Fh1NativeExecutor::LayoutConstant(const Surface& surface) const {
  const SurfaceKey& key = surface.key;
  const uint32_t host_sample_mode = HostSampleMode(surface);
  return PackLayout(key.base_tiles, key.pitch_tiles, key.msaa, key.Is64bpp(), key.is_depth,
                    key.format) |
         (host_sample_mode << 27);
}

VkShaderModule Fh1NativeExecutor::GetShaderModule(const uint32_t* code, size_t size_bytes) {
  auto it = shader_modules_.find(code);
  if (it != shader_modules_.end()) return it->second;
  VkShaderModule module = ui::vulkan::util::CreateShaderModule(
      command_processor_.GetVulkanDevice(), code, size_bytes);
  if (module) shader_modules_.emplace(code, module);
  return module;
}

VkPipeline Fh1NativeExecutor::GetTransferPipeline(const TransferPipelineKey& key) {
  auto it = transfer_pipelines_.find(key);
  if (it != transfer_pipelines_.end()) return it->second;
  // Depth with stencil exported by the shader has the depth pass's state; the
  // exported value takes the place of the stencil reference.
  const bool exports_stencil = key.dest_kind == kTransferDestDepthStencil;
  const uint32_t kind = exports_stencil                   ? 1
                        : key.dest_kind == kTransferDestUint ? 3
                                                             : std::min(key.dest_kind, 2u);
  const SpirvShader& fragment =
      exports_stencil
          ? kTransferDepthStencilShaders[key.dest_samples > 1][key.source_kind][key.source_msaa]
      : key.source_kind == kTransferSourceWords
          ? kFromWordsShaders[kind == 1 ? 0 : 1][key.dest_samples > 1]
          : kTransferShaders[kind][key.dest_samples > 1][key.source_kind][key.source_msaa];
  VkShaderModule vertex_module =
      GetShaderModule(shaders::fullscreen_cw_vs, sizeof(shaders::fullscreen_cw_vs));
  VkShaderModule fragment_module = GetShaderModule(fragment.code, fragment.size);
  if (!vertex_module || !fragment_module) return VK_NULL_HANDLE;

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_module;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragment_module;
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization = {};
  rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_NONE;
  rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VkSampleCountFlagBits(key.dest_samples);
  const VkSampleMask sample_mask = key.sample_mask;
  multisample.pSampleMask = &sample_mask;
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
  depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  VkPipelineColorBlendAttachmentState blend_attachment = {};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  VkPipelineRenderingCreateInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  if (kind == 0 || kind == 3) {
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &key.dest_format;
  } else {
    rendering.depthAttachmentFormat = key.dest_format;
    rendering.stencilAttachmentFormat = key.dest_format;
    depth_stencil.stencilTestEnable = VK_TRUE;
    depth_stencil.front.failOp = VK_STENCIL_OP_KEEP;
    depth_stencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
    depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
    depth_stencil.front.compareMask = 0xFF;
    if (kind == 1) {
      // Depth, and stencil reset to 0 (reference 0).
      depth_stencil.depthTestEnable = VK_TRUE;
      depth_stencil.depthWriteEnable = VK_TRUE;
      depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
      depth_stencil.front.writeMask = 0xFF;
    } else {
      // One stencil bit where the source has it (reference 0xFF).
      depth_stencil.front.writeMask = 1u << (key.dest_kind - 2);
    }
    depth_stencil.back = depth_stencil.front;
  }
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                           VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &rasterization;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = key.source_kind == kTransferSourceWords ? words_pipeline_layout_
                                                        : transfer_pipeline_layout_;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  VkPipeline pipeline;
  if (vulkan_device->functions().vkCreateGraphicsPipelines(vulkan_device->device(),
                                                           VK_NULL_HANDLE, 1, &info, nullptr,
                                                           &pipeline) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return transfer_pipelines_.emplace(key, pipeline).first->second;
}

VkPipeline Fh1NativeExecutor::GetComputePipeline(bool words, uint32_t source_kind, bool msaa) {
  VkPipeline& pipeline = compute_pipelines_[words][source_kind][msaa];
  if (pipeline) return pipeline;
  const SpirvShader& shader = kComputeShaders[words][source_kind][msaa];
  pipeline = ui::vulkan::util::CreateComputePipeline(command_processor_.GetVulkanDevice(),
                                                     compute_pipeline_layout_, shader.code,
                                                     shader.size);
  return pipeline;
}

VkDescriptorSet Fh1NativeExecutor::AllocateDescriptorSet(VkDescriptorSetLayout layout) {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // A frame reopens a slot only after the frame kDescriptorPoolFrames earlier
  // completed (the command processor keeps no more frames in flight).
  const uint64_t frame = command_processor_.GetCurrentFrame();
  const uint32_t slot = uint32_t(frame % kDescriptorPoolFrames);
  auto& pools = descriptor_pools_[slot];
  if (descriptor_pool_frames_[slot] != frame) {
    for (VkDescriptorPool pool : pools) dfn.vkResetDescriptorPool(device, pool, 0);
    descriptor_pool_frames_[slot] = frame;
    descriptor_pool_current_[slot] = 0;
  }
  for (;;) {
    uint32_t& current = descriptor_pool_current_[slot];
    if (current >= pools.size()) {
      const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512}};
      VkDescriptorPoolCreateInfo info = {};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      info.maxSets = 512;
      info.poolSizeCount = uint32_t(std::size(sizes));
      info.pPoolSizes = sizes;
      VkDescriptorPool pool;
      if (dfn.vkCreateDescriptorPool(device, &info, nullptr, &pool) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
      }
      pools.push_back(pool);
    }
    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pools[current];
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    VkDescriptorSet set;
    const VkResult result = dfn.vkAllocateDescriptorSets(device, &allocate, &set);
    if (result == VK_SUCCESS) return set;
    if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
      return VK_NULL_HANDLE;
    }
    ++current;
  }
}

void Fh1NativeExecutor::TransferTiles(Surface& dest, const TileRun& run) {
  const uint32_t msaa_x_log2 = uint32_t(dest.key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(dest.key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t dest_64bpp = dest.key.Is64bpp() ? 1 : 0;
  const uint32_t tile_width = (xenos::kEdramTileWidthSamples >> msaa_x_log2) >> dest_64bpp;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  const uint32_t pitch = dest.key.pitch_tiles << dest_64bpp;
  const uint32_t first = (run.first - dest.key.base_tiles) & (xenos::kEdramTileCount - 1);
  const uint32_t end = first + run.count;
  Rect rects[3];
  uint32_t rect_count = 0;
  auto add_rect = [&](uint32_t column_first, uint32_t row_first, uint32_t column_end,
                      uint32_t row_end) {
    Rect rect = {int32_t(column_first * tile_width), int32_t(row_first * tile_height),
                 int32_t(std::min(column_end * tile_width, dest.width)),
                 int32_t(std::min(row_end * tile_height, dest.height))};
    if (rect.left < rect.right && rect.top < rect.bottom) rects[rect_count++] = HostRect(rect);
  };
  const uint32_t first_row = first / pitch, last_row = (end - 1) / pitch;
  if (first_row == last_row) {
    add_rect(first % pitch, first_row, (end - 1) % pitch + 1, first_row + 1);
  } else {
    uint32_t full_first = first_row;
    if (first % pitch) {
      add_rect(first % pitch, first_row, pitch, first_row + 1);
      ++full_first;
    }
    uint32_t full_end = last_row + 1;
    if (end % pitch) {
      --full_end;
      add_rect(0, last_row, end % pitch, last_row + 1);
    }
    if (full_first < full_end) add_rect(0, full_first, pitch, full_end);
  }
  if (rect_count) {
    TransferRects(dest, run.previous_owner, rects, rect_count, run.count,
                  tiles_.AnyStencil(run.first, run.count));
  }
}

void Fh1NativeExecutor::ClaimTileRect(const SurfaceKey& key, uint32_t column_first,
                                      uint32_t row_first, uint32_t column_end,
                                      uint32_t row_end) {
  const uint32_t packed_key = key.Pack();
  const auto claim = tiles_.ClaimRect(key.base_tiles, key.pitch_tiles, packed_key, column_first,
                                      row_first, column_end, row_end);
  if (claim.per_row) {
    for (uint32_t row = row_first; row < row_end; ++row) {
      ClaimTiles(key.base_tiles + row * key.pitch_tiles + column_first,
                 column_end - column_first, packed_key);
    }
    return;
  }
  const uint32_t previous_owner = claim.previous_owner;
  if (previous_owner == kNoOwner) return;
  Surface* dest = FindSurface(packed_key);
  if (!dest) return;
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> msaa_x_log2;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  const Rect rect = {int32_t(column_first * tile_width), int32_t(row_first * tile_height),
                     int32_t(std::min(column_end * tile_width, dest->width)),
                     int32_t(std::min(row_end * tile_height, dest->height))};
  if (rect.left < rect.right && rect.top < rect.bottom) {
    bool tiles_stencil = false;
    for (uint32_t row = row_first; row < row_end && !tiles_stencil; ++row) {
      tiles_stencil = tiles_.AnyStencil(key.base_tiles + row * key.pitch_tiles + column_first,
                                        column_end - column_first);
    }
    const Rect host_rect = HostRect(rect);
    TransferRects(*dest, previous_owner, &host_rect, 1,
                  (column_end - column_first) * (row_end - row_first), tiles_stencil);
  }
}

void Fh1NativeExecutor::TransferRects(Surface& dest, uint32_t previous_owner, const Rect* rects,
                                      uint32_t rect_count, uint32_t tile_count,
                                      bool tiles_stencil) {
  GpuTimer gpu_timer(*this, kGpuTransfers);
  command_processor_.Checkpoint(VulkanCommandProcessor::CheckpointKind::kTransfer);
  Surface* source = FindSurface(previous_owner);
  if (!source) return Skip("transfer_source_missing");
  if ((!dest.key.is_depth &&
       !Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(dest.key.format))) ||
      (!source->key.is_depth &&
       !Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(source->key.format)))) {
    return Skip("transfer_format");
  }
  Count("transfer");
  const bool source_stencil =
      source->key.is_depth ? source->stencil_nonzero && tiles_stencil : true;
  uint32_t pass_count = 1;
  if (dest.key.is_depth) {
    if (!EnsureTransferWords(dest)) return Skip("transfer_words_buffer");
    dest.stencil_nonzero |= source_stencil;
    pass_count = source_stencil ? 9 : 1;
    if (!source_stencil) Count("transfer_stencil_skipped");
  }
  counters_.Count("transfer_tile_passes", uint64_t(tile_count) * pass_count);
  if (gpu_query_pool_ != VK_NULL_HANDLE) {
    transfer_volume_[source->key.Describe() + "->" + dest.key.Describe()] +=
        uint64_t(tile_count) * pass_count;
  }
  for (uint32_t i = 0; i < rect_count; ++i) {
    pending_transfers_.push_back({dest.key.Pack(), previous_owner, rects[i], source_stencil});
  }
}

void Fh1NativeExecutor::FlushTransfers() {
  GpuTimer gpu_timer(*this, kGpuTransfers);
  command_processor_.Checkpoint(VulkanCommandProcessor::CheckpointKind::kTransfer);
  if (pending_transfers_.empty()) return;
  std::stable_sort(pending_transfers_.begin(), pending_transfers_.end(),
                   [](const PendingTransfer& a, const PendingTransfer& b) {
                     return a.dest != b.dest ? a.dest < b.dest : a.source < b.source;
                   });
  for (size_t group = 0; group < pending_transfers_.size();) {
    size_t group_end = group;
    while (group_end < pending_transfers_.size() &&
           pending_transfers_[group_end].dest == pending_transfers_[group].dest) {
      ++group_end;
    }
    if (Surface* dest = FindSurface(pending_transfers_[group].dest)) {
      if (dest->key.is_depth) {
        FlushDepthTransfers(*dest, group, group_end);
      } else {
        FlushColorTransfers(*dest, group, group_end);
      }
    }
    group = group_end;
  }
  pending_transfers_.clear();
}

void Fh1NativeExecutor::FlushColorTransfers(Surface& dest, size_t first, size_t end) {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (size_t i = first; i < end; ++i) {
    if (Surface* source = FindSurface(pending_transfers_[i].source)) {
      TransitionForSampling(*source, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
  }
  TransitionForAttachment(dest);
  const bool dest_uint = dest.uint_view != VK_NULL_HANDLE;
  BeginSurfaceRendering(dest, dest_uint);
  auto& command_buffer = command_processor_.deferred_command_buffer();
  const uint32_t flags = TransferFlags();
  const uint32_t sample_mask =
      dest.key.msaa == uint32_t(xenos::MsaaSamples::k2X) && dest.samples == 4 ? 0b1001u
                                                                             : UINT32_MAX;
  for (size_t i = first; i < end;) {
    size_t source_end = i;
    while (source_end < end &&
           pending_transfers_[source_end].source == pending_transfers_[i].source) {
      ++source_end;
    }
    Surface* source = FindSurface(pending_transfers_[i].source);
    VkDescriptorSet set = source ? AllocateDescriptorSet(transfer_set_layout_) : VK_NULL_HANDLE;
    if (!set) {
      Skip("transfer_descriptor");
      i = source_end;
      continue;
    }
    VkDescriptorImageInfo images[2] = {
        {VK_NULL_HANDLE, source->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, source->stencil_view ? source->stencil_view : source->sampled_view,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t w = 0; w < 2; ++w) {
      writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[w].dstSet = set;
      writes[w].dstBinding = w ? kBindingStencil : kBindingSource;
      writes[w].descriptorCount = 1;
      writes[w].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[w].pImageInfo = &images[w];
    }
    dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    TransferPipelineKey key;
    key.dest_kind = dest_uint ? kTransferDestUint : 0;
    key.dest_format = dest_uint ? dest.uint_format : dest.format;
    key.dest_samples = dest.samples;
    key.sample_mask = sample_mask;
    key.source_kind = SourceKind(source->key.is_depth, source->key.format);
    key.source_msaa = source->samples > 1;
    VkPipeline pipeline = GetTransferPipeline(key);
    if (!pipeline) {
      Skip("transfer_pipeline");
      i = source_end;
      continue;
    }
    command_processor_.BindExternalGraphicsPipeline(pipeline);
    command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                           transfer_pipeline_layout_, 0, 1, &set, 0, nullptr);
    const uint32_t constants[kTransferConstantCount] = {LayoutConstant(dest),
                                                        LayoutConstant(*source), flags};
    command_buffer.CmdVkPushConstants(transfer_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(constants), constants);
    for (; i < source_end; ++i) {
      const Rect& rect = pending_transfers_[i].rect;
      command_processor_.SetViewport({float(rect.left), float(rect.top),
                                      float(rect.right - rect.left),
                                      float(rect.bottom - rect.top), 0.0f, 1.0f});
      command_processor_.SetScissor(
          {{rect.left, rect.top},
           {uint32_t(rect.right - rect.left), uint32_t(rect.bottom - rect.top)}});
      command_buffer.CmdVkDraw(3, 1, 0, 0);
    }
  }
  Count("transfer_batch");
}

// With stencil export, depth and stencil in one pass per source, read
// straight from it as color transfers are: no EDRAM words buffer, no compute
// pass, no pass per stencil bit.
void Fh1NativeExecutor::FlushDepthTransfersExported(Surface& dest, size_t first, size_t end) {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (size_t i = first; i < end; ++i) {
    if (Surface* source = FindSurface(pending_transfers_[i].source)) {
      TransitionForSampling(*source, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
  }
  TransitionForAttachment(dest);
  BeginSurfaceRendering(dest, false);
  auto& command_buffer = command_processor_.deferred_command_buffer();
  const uint32_t flags = TransferFlags();
  const uint32_t sample_mask =
      dest.key.msaa == uint32_t(xenos::MsaaSamples::k2X) && dest.samples == 4 ? 0b1001u
                                                                             : UINT32_MAX;
  // The exported value replaces the reference, which still has to be set.
  command_buffer.CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  for (size_t i = first; i < end;) {
    size_t source_end = i;
    while (source_end < end &&
           pending_transfers_[source_end].source == pending_transfers_[i].source) {
      ++source_end;
    }
    Surface* source = FindSurface(pending_transfers_[i].source);
    VkDescriptorSet set = source ? AllocateDescriptorSet(transfer_set_layout_) : VK_NULL_HANDLE;
    if (!set) {
      Skip("transfer_descriptor");
      i = source_end;
      continue;
    }
    VkDescriptorImageInfo images[2] = {
        {VK_NULL_HANDLE, source->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, source->stencil_view ? source->stencil_view : source->sampled_view,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t w = 0; w < 2; ++w) {
      writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[w].dstSet = set;
      writes[w].dstBinding = w ? kBindingStencil : kBindingSource;
      writes[w].descriptorCount = 1;
      writes[w].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[w].pImageInfo = &images[w];
    }
    dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    TransferPipelineKey key;
    key.dest_kind = kTransferDestDepthStencil;
    key.dest_format = dest.format;
    key.dest_samples = dest.samples;
    key.sample_mask = sample_mask;
    key.source_kind = SourceKind(source->key.is_depth, source->key.format);
    key.source_msaa = source->samples > 1;
    VkPipeline pipeline = GetTransferPipeline(key);
    if (!pipeline) {
      Skip("transfer_pipeline");
      i = source_end;
      continue;
    }
    command_processor_.BindExternalGraphicsPipeline(pipeline);
    command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                           transfer_pipeline_layout_, 0, 1, &set, 0, nullptr);
    const uint32_t constants[kTransferConstantCount] = {LayoutConstant(dest),
                                                        LayoutConstant(*source), flags};
    command_buffer.CmdVkPushConstants(transfer_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(constants), constants);
    for (; i < source_end; ++i) {
      const Rect& rect = pending_transfers_[i].rect;
      command_processor_.SetViewport({float(rect.left), float(rect.top),
                                      float(rect.right - rect.left),
                                      float(rect.bottom - rect.top), 0.0f, 1.0f});
      command_processor_.SetScissor(
          {{rect.left, rect.top},
           {uint32_t(rect.right - rect.left), uint32_t(rect.bottom - rect.top)}});
      command_buffer.CmdVkDraw(3, 1, 0, 0);
    }
  }
  Count("transfer_batch");
  Count("transfer_stencil_exported");
}

bool Fh1NativeExecutor::EnsureTransferWords(const Surface& dest) {
  const VkDeviceSize size = VkDeviceSize(dest.width) * dest.height * scale_ * scale_ *
                            dest.samples * sizeof(uint32_t);
  if (transfer_words_ && transfer_words_size_ >= size) return true;
  if (transfer_words_) {
    retired_buffers_.emplace_back(transfer_words_, transfer_words_memory_);
    transfer_words_ = VK_NULL_HANDLE;
    transfer_words_memory_ = VK_NULL_HANDLE;
  }
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(), size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, transfer_words_,
          transfer_words_memory_)) {
    transfer_words_size_ = 0;
    return false;
  }
  transfer_words_size_ = size;
  return true;
}

void Fh1NativeExecutor::FlushDepthTransfers(Surface& dest, size_t first, size_t end) {
  if (stencil_export_) {
    FlushDepthTransfersExported(dest, first, end);
    return;
  }
  if (!EnsureTransferWords(dest)) {
    Skip("transfer_words_buffer");
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  auto& command_buffer = command_processor_.deferred_command_buffer();
  const uint32_t flags = TransferFlags();

  // The EDRAM words of every rectangle, one source at a time, by compute.
  for (size_t i = first; i < end; ++i) {
    if (Surface* source = FindSurface(pending_transfers_[i].source)) {
      TransitionForSampling(*source, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
  }
  command_processor_.PushBufferMemoryBarrier(
      transfer_words_, 0, VK_WHOLE_SIZE,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  command_processor_.SubmitBarriers(true);
  bool any_stencil = false;
  for (size_t i = first; i < end; ++i) any_stencil |= pending_transfers_[i].stencil;
  for (size_t i = first; i < end;) {
    size_t source_end = i;
    while (source_end < end &&
           pending_transfers_[source_end].source == pending_transfers_[i].source) {
      ++source_end;
    }
    Surface* source = FindSurface(pending_transfers_[i].source);
    VkPipeline pipeline =
        source ? GetComputePipeline(true, SourceKind(source->key.is_depth, source->key.format),
                                    source->samples > 1)
               : VK_NULL_HANDLE;
    VkDescriptorSet set = pipeline ? AllocateDescriptorSet(compute_set_layout_) : VK_NULL_HANDLE;
    if (!set) {
      Skip("transfer_words_pipeline");
      i = source_end;
      continue;
    }
    VkDescriptorImageInfo images[2] = {
        {VK_NULL_HANDLE, source->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, source->stencil_view ? source->stencil_view : source->sampled_view,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkDescriptorBufferInfo buffer = {transfer_words_, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t w = 0; w < 3; ++w) {
      writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[w].dstSet = set;
      writes[w].descriptorCount = 1;
    }
    writes[0].dstBinding = kBindingSource;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &images[0];
    writes[1].dstBinding = kBindingStencil;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[1].pImageInfo = &images[1];
    writes[2].dstBinding = kBindingMemory;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &buffer;
    dfn.vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    command_processor_.BindExternalComputePipeline(pipeline);
    command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                           compute_pipeline_layout_, 0, 1, &set, 0, nullptr);
    command_processor_.BeginDebugLabel("fh1 transfer words");
    struct EndLabel {
      VulkanCommandProcessor& processor;
      ~EndLabel() { processor.EndDebugLabel(); }
    } end_label{command_processor_};
    for (; i < source_end; ++i) {
      const Rect& rect = pending_transfers_[i].rect;
      const uint32_t constants[kComputeConstantCount] = {
          uint32_t(rect.left) | (uint32_t(rect.top) << 16),
          uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16),
          LayoutConstant(dest), LayoutConstant(*source), flags, dest.width * scale_,
          dest.samples, 0};
      command_buffer.CmdVkPushConstants(compute_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                        sizeof(constants), constants);
      command_buffer.CmdVkDispatch((uint32_t(rect.right - rect.left) + 7) / 8,
                                   (uint32_t(rect.bottom - rect.top) + 7) / 8, 1);
    }
  }
  command_processor_.PushBufferMemoryBarrier(
      transfer_words_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);

  // The depth pass (which resets stencil to 0), then one pass per stencil bit
  // over the rectangles whose source may have nonzero stencil.
  TransitionForAttachment(dest);
  VkDescriptorSet words_set = AllocateDescriptorSet(words_set_layout_);
  if (!words_set) {
    Skip("transfer_descriptor");
    return;
  }
  VkDescriptorBufferInfo words_buffer = {transfer_words_, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = words_set;
  write.dstBinding = kBindingSource;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &words_buffer;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  BeginSurfaceRendering(dest, false);
  const uint32_t sample_mask =
      dest.key.msaa == uint32_t(xenos::MsaaSamples::k2X) && dest.samples == 4 ? 0b1001u
                                                                             : UINT32_MAX;
  bool set_bound = false;
  for (uint32_t pass = 0; pass < (any_stencil ? 9u : 1u); ++pass) {
    TransferPipelineKey key;
    key.dest_kind = pass ? 1 + pass : 1;
    key.dest_format = dest.format;
    key.dest_samples = dest.samples;
    key.sample_mask = sample_mask;
    key.source_kind = kTransferSourceWords;
    key.source_msaa = false;
    VkPipeline pipeline = GetTransferPipeline(key);
    if (!pipeline) {
      Skip("transfer_pipeline");
      break;
    }
    command_processor_.BindExternalGraphicsPipeline(pipeline);
    if (!set_bound) {
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                             words_pipeline_layout_, 0, 1, &words_set, 0,
                                             nullptr);
      set_bound = true;
    }
    const uint32_t constants[kTransferConstantCount] = {
        LayoutConstant(dest), (dest.width * scale_) | (dest.samples << 16),
        flags | (pass ? (pass - 1) << 8 : 0)};
    command_buffer.CmdVkPushConstants(words_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(constants), constants);
    command_buffer.CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_AND_BACK, pass ? 0xFF : 0);
    for (size_t i = first; i < end; ++i) {
      if (pass && !pending_transfers_[i].stencil) continue;
      const Rect& rect = pending_transfers_[i].rect;
      command_processor_.SetViewport({float(rect.left), float(rect.top),
                                      float(rect.right - rect.left),
                                      float(rect.bottom - rect.top), 0.0f, 1.0f});
      command_processor_.SetScissor(
          {{rect.left, rect.top},
           {uint32_t(rect.right - rect.left), uint32_t(rect.bottom - rect.top)}});
      command_buffer.CmdVkDraw(3, 1, 0, 0);
    }
  }
  Count("transfer_batch");
}

void Fh1NativeExecutor::GetResolveSources(const SurfaceKey& resolve_key, int32_t x0, int32_t y0,
                                          int32_t x1, int32_t y1,
                                          std::vector<SourceRect>& sources_out) {
  sources_out.clear();
  const uint32_t msaa_x_log2 = uint32_t(resolve_key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(resolve_key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t is_64bpp = resolve_key.Is64bpp() ? 1 : 0;
  const int32_t tile_width = int32_t((xenos::kEdramTileWidthSamples >> msaa_x_log2) >> is_64bpp);
  const int32_t tile_height = int32_t(xenos::kEdramTileHeightSamples >> msaa_y_log2);
  for (const auto& rect : tiles_.SplitByOwner(resolve_key.base_tiles,
                                              resolve_key.pitch_tiles << is_64bpp, tile_width,
                                              tile_height, x0, y0, x1, y1)) {
    SourceRect source;
    source.rect = {rect.left, rect.top, rect.right, rect.bottom};
    source.unowned = rect.owner == kNoOwner;
    source.surface = source.unowned ? nullptr : FindSurface(rect.owner);
    sources_out.push_back(source);
  }
}

bool Fh1NativeExecutor::DrawMayWriteNonzeroStencil(reg::RB_DEPTHCONTROL depth_control) const {
  if (!depth_control.stencil_enable) return false;
  const RegisterFile& regs = register_file_;
  auto may_write = [](xenos::StencilOp op, uint32_t reference) {
    switch (op) {
      case xenos::StencilOp::kKeep:
      case xenos::StencilOp::kZero:
        return false;
      case xenos::StencilOp::kReplace:
        return reference != 0;
      default:
        return true;
    }
  };
  auto face = [&](const reg::RB_STENCILREFMASK& mask, xenos::StencilOp fail,
                  xenos::StencilOp zfail, xenos::StencilOp zpass) {
    const uint32_t reference = mask.stencilref & mask.stencilwritemask;
    return mask.stencilwritemask != 0 &&
           (may_write(fail, reference) || may_write(zfail, reference) ||
            may_write(zpass, reference));
  };
  const auto front = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK);
  if (face(front, depth_control.stencilfail, depth_control.stencilzfail,
           depth_control.stencilzpass)) {
    return true;
  }
  return depth_control.backface_enable &&
         face(regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF),
              depth_control.stencilfail_bf, depth_control.stencilzfail_bf,
              depth_control.stencilzpass_bf);
}

bool Fh1NativeExecutor::ClaimDepthOverwriteTiles(const SurfaceKey& key, uint32_t length,
                                                 bool stencil_overwritten, bool stencil_written) {
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> msaa_x_log2;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  for (const auto& rect : depth_overwrite_.rects()) {
    const uint32_t column_end = (uint32_t(rect.outer[2]) + tile_width - 1) / tile_width;
    const uint32_t row_end = (uint32_t(rect.outer[3]) + tile_height - 1) / tile_height;
    if (column_end > key.pitch_tiles || (row_end - 1) * key.pitch_tiles + column_end > length) {
      return false;
    }
  }
  for (const auto& rect : depth_overwrite_.rects()) {
    ClaimOverwrittenDepthTiles(key, rect.inner, stencil_overwritten);
    const uint32_t column_first = uint32_t(rect.outer[0]) / tile_width;
    const uint32_t column_end = (uint32_t(rect.outer[2]) + tile_width - 1) / tile_width;
    const uint32_t row_first = uint32_t(rect.outer[1]) / tile_height;
    const uint32_t row_end = (uint32_t(rect.outer[3]) + tile_height - 1) / tile_height;
    uint32_t inner_column_first = (uint32_t(rect.inner[0]) + tile_width - 1) / tile_width;
    uint32_t inner_column_end = uint32_t(rect.inner[2]) / tile_width;
    uint32_t inner_row_first = (uint32_t(rect.inner[1]) + tile_height - 1) / tile_height;
    uint32_t inner_row_end = uint32_t(rect.inner[3]) / tile_height;
    if (inner_column_first >= inner_column_end || inner_row_first >= inner_row_end) {
      inner_column_first = inner_column_end = column_first;
      inner_row_first = inner_row_end = row_first;
    }
    ClaimTileRect(key, column_first, row_first, column_end, inner_row_first);
    ClaimTileRect(key, column_first, inner_row_end, column_end, row_end);
    ClaimTileRect(key, column_first, inner_row_first, inner_column_first, inner_row_end);
    ClaimTileRect(key, inner_column_end, inner_row_first, column_end, inner_row_end);
    ClaimTileRect(key, inner_column_first, inner_row_first, inner_column_end, inner_row_end);
    if (stencil_written) {
      for (uint32_t row = row_first; row < row_end; ++row) {
        MarkTileStencil(key.base_tiles + row * key.pitch_tiles + column_first,
                        column_end - column_first, true);
      }
    }
  }
  Count("depth_overwrite_draw");
  return true;
}

void Fh1NativeExecutor::ClaimOverwrittenDepthTiles(const SurfaceKey& key,
                                                   const std::array<int32_t, 4>& rect,
                                                   bool stencil_overwritten) {
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> msaa_x_log2;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  const uint32_t column_first = (uint32_t(rect[0]) + tile_width - 1) / tile_width;
  const uint32_t column_end = std::min(uint32_t(rect[2]) / tile_width, key.pitch_tiles);
  const uint32_t row_first = (uint32_t(rect[1]) + tile_height - 1) / tile_height;
  const uint32_t row_end = uint32_t(rect[3]) / tile_height;
  if (column_first >= column_end || row_first >= row_end) return;
  const uint32_t packed_key = key.Pack();
  Surface* dest = FindSurface(packed_key);
  if (!dest) return;
  if (!stencil_overwritten) {
    for (uint32_t row = row_first; row < row_end; ++row) {
      for (uint32_t column = column_first; column < column_end; ++column) {
        if (tiles_.StencilNonzero(key.base_tiles + row * key.pitch_tiles + column)) return;
      }
    }
  }
  for (uint32_t row = row_first; row < row_end; ++row) {
    ClaimTiles(key.base_tiles + row * key.pitch_tiles + column_first, column_end - column_first,
               packed_key, false);
  }
  if (!stencil_overwritten && dest->stencil_nonzero) {
    const Rect clear_rect =
        HostRect({int32_t(column_first * tile_width), int32_t(row_first * tile_height),
                  int32_t(std::min(column_end * tile_width, dest->width)),
                  int32_t(std::min(row_end * tile_height, dest->height))});
    TransitionForAttachment(*dest);
    BeginSurfaceRendering(*dest, false);
    VkClearAttachment clear = {};
    clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    VkClearRect vk_rect = {};
    vk_rect.rect = {{clear_rect.left, clear_rect.top},
                    {uint32_t(clear_rect.right - clear_rect.left),
                     uint32_t(clear_rect.bottom - clear_rect.top)}};
    vk_rect.layerCount = 1;
    command_processor_.deferred_command_buffer().CmdVkClearAttachments(1, &clear, 1, &vk_rect);
  }
  Count("depth_overwrite_claim");
}

void Fh1NativeExecutor::PrepareTargets(const Fh1DrawInfo& draw) {
  if (!initialized_) return;
  // The extent estimate reads vertices when clipping is off.
  const bool memo_allowed = draw.state_epoch && !draw.memexport &&
                            !register_file_.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  PrepareMemo& memo = prepare_memo_;
  if (memo_allowed && memo.state_epoch == draw.state_epoch &&
      memo.vertex_shader == draw.vertex_shader &&
      memo.depth_control == draw.normalized_depth_control.value &&
      memo.color_mask == draw.normalized_color_mask &&
      memo.rasterization_done == draw.rasterization_done &&
      memo.generation == tiles_.generation()) {
    pending_targets_valid_ = true;
    pending_used_bits_ = memo.used_bits;
    std::memcpy(pending_keys_, memo.keys, sizeof(memo.keys));
    return;
  }
  PrepareTargetsImpl(draw);
  const bool may_overwrite_depth =
      pending_used_bits_ == 1 && draw.normalized_depth_control.z_enable &&
      draw.normalized_depth_control.z_write_enable &&
      draw.normalized_depth_control.zfunc == xenos::CompareFunction::kAlways;
  if (memo_allowed && !may_overwrite_depth) {
    memo.state_epoch = draw.state_epoch;
    memo.vertex_shader = draw.vertex_shader;
    memo.depth_control = draw.normalized_depth_control.value;
    memo.color_mask = draw.normalized_color_mask;
    memo.rasterization_done = draw.rasterization_done;
    memo.generation = tiles_.generation();
    memo.used_bits = pending_used_bits_;
    std::memcpy(memo.keys, pending_keys_, sizeof(memo.keys));
  } else {
    memo.state_epoch = 0;
  }
}

void Fh1NativeExecutor::PrepareTargetsImpl(const Fh1DrawInfo& draw) {
  pending_targets_valid_ = true;
  pending_used_bits_ = 0;
  if (draw.memexport) return;
  const RegisterFile& regs = register_file_;
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t msaa = uint32_t(surface_info.msaa_samples);
  const uint32_t pitch_tiles = Fh1PitchTiles(surface_info.surface_pitch, msaa);
  uint32_t used_bits = 0;
  SurfaceKey* keys = pending_keys_;
  if (draw.rasterization_done && pitch_tiles) {
    if (draw.normalized_depth_control.z_enable || draw.normalized_depth_control.stencil_enable) {
      const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      keys[0] = MakeDepthKey(depth_info.depth_base, pitch_tiles, msaa, depth_info.depth_format);
      used_bits |= 1;
    }
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(draw.normalized_color_mask & (0xFu << (4 * i)))) continue;
      const auto color_info =
          regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      keys[1 + i] = MakeColorKey(color_info.color_base, pitch_tiles, msaa, color_info.color_format);
      used_bits |= 1u << (1 + i);
    }
    for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (!(used_bits & (1u << i))) continue;
      for (uint32_t j = 0; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
        if (j != i && (j == 0 || j > i) && (used_bits & (1u << j)) &&
            keys[j].base_tiles == keys[i].base_tiles) {
          used_bits &= ~(1u << j);
        }
      }
    }
  }
  pending_used_bits_ = used_bits;
  if (!used_bits) return;

  const uint32_t msaa_y_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k2X));
  auto length_for_height = [&](uint32_t height) {
    return ((height << msaa_y_log2) + xenos::kEdramTileHeightSamples - 1) /
           xenos::kEdramTileHeightSamples * pitch_tiles;
  };
  const uint32_t surface_height = SurfaceHeight(pitch_tiles, msaa);
  const bool depth_stencil_written =
      (used_bits & 1) && DrawMayWriteNonzeroStencil(draw.normalized_depth_control);
  const bool may_overwrite_depth =
      used_bits == 1 && draw.normalized_depth_control.z_enable &&
      draw.normalized_depth_control.z_write_enable &&
      draw.normalized_depth_control.zfunc == xenos::CompareFunction::kAlways;
  PrepareSignature signature;
  signature.generation = tiles_.generation();
  signature.used_bits = used_bits;
  signature.stencil_written = depth_stencil_written;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (used_bits & (1u << i)) signature.keys[i] = keys[i].Pack();
  }
  // With no tile changed since a recent preparation of the same targets, a
  // claim no longer than that one is already in place; when that one covered
  // the whole surface, the draw's extent need not even be estimated. Draws
  // alternating between a few target sets all hit.
  const PrepareSignature* same_targets = nullptr;
  if (!may_overwrite_depth) {
    for (const PrepareSignature& recent : recent_prepares_) {
      if (recent.length_tiles && signature.SameTargets(recent)) {
        same_targets = &recent;
        break;
      }
    }
  }
  if (same_targets && same_targets->length_tiles >= length_for_height(surface_height)) return;
  const uint32_t height_used =
      std::min(surface_height, draw.vertex_shader
                                   ? draw_extent_estimator_.EstimateMaxY(true, *draw.vertex_shader)
                                   : surface_height);
  const uint32_t length_tiles_32bpp = length_for_height(height_used);
  signature.length_tiles = length_tiles_32bpp;
  if (same_targets && length_tiles_32bpp <= same_targets->length_tiles) return;

  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if ((used_bits & (1u << i)) && !GetOrCreateSurface(keys[i])) used_bits &= ~(1u << i);
  }
  pending_used_bits_ = used_bits;
  if (depth_stencil_written && (used_bits & 1)) {
    FindSurface(keys[0].Pack())->stencil_nonzero = true;
  }
  std::vector<std::pair<uint32_t, uint32_t>> bases;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (used_bits & (1u << i)) bases.emplace_back(keys[i].base_tiles, i);
  }
  std::sort(bases.begin(), bases.end());
  bool stencil_overwritten = false;
  const bool depth_overwritten =
      used_bits == 1 &&
      depth_overwrite_.Derive(draw, REXCVAR_GET(half_pixel_offset), stencil_overwritten);
  for (size_t i = 0; i < bases.size(); ++i) {
    const SurfaceKey& key = keys[bases[i].second];
    const uint32_t next_base = i + 1 < bases.size()
                                   ? bases[i + 1].first
                                   : xenos::kEdramTileCount + bases[0].first;
    const uint32_t length =
        std::min(length_tiles_32bpp << uint32_t(key.Is64bpp()), next_base - key.base_tiles);
    if (depth_overwritten &&
        ClaimDepthOverwriteTiles(key, length, stencil_overwritten, depth_stencil_written)) {
      continue;
    }
    ClaimTiles(key.base_tiles, length, key.Pack());
    if (!key.is_depth || depth_stencil_written) MarkTileStencil(key.base_tiles, length, true);
  }
  FlushTransfers();
  if (used_bits == signature.used_bits && !may_overwrite_depth) {
    // Entries of an older generation never match, so no invalidation is
    // needed; the same targets take their old entry's place.
    signature.generation = tiles_.generation();
    PrepareSignature* slot = nullptr;
    for (PrepareSignature& recent : recent_prepares_) {
      if (recent.used_bits == signature.used_bits &&
          !std::memcmp(recent.keys, signature.keys, sizeof(signature.keys))) {
        slot = &recent;
        break;
      }
    }
    if (!slot) {
      slot = &recent_prepares_[recent_prepare_next_++ % std::size(recent_prepares_)];
    }
    *slot = signature;
  }
}

bool Fh1NativeExecutor::BindTargets(VulkanRenderTargetCache::RenderPassKey& key_out) {
  key_out = VulkanRenderTargetCache::RenderPassKey();
  bound_bits_ = 0;
  attachment_barriers_skipped_ = false;
  if (!initialized_) return false;
  if (!pending_targets_valid_) {
    Skip("draw_targets_not_prepared");
    return false;
  }
  const uint32_t used_bits = pending_used_bits_;
  const SurfaceKey* keys = pending_keys_;
  const uint64_t rendering_id = DrawRenderingId(used_bits);
  key_out.msaa_samples = single_sample_msaa_
                             ? xenos::MsaaSamples::k1X
                             : register_file_.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(used_bits & (1u << i))) continue;
    Surface* surface = FindSurface(keys[i].Pack());
    if (!surface) {
      Skip("draw_surface_create");
      return false;
    }
    TransitionForAttachment(*surface, rendering_id);
    switch (i) {
      case 0:
        key_out.depth_format = xenos::DepthRenderTargetFormat(keys[0].format);
        break;
      case 1:
        key_out.color_0_view_format = xenos::ColorRenderTargetFormat(keys[1].format);
        break;
      case 2:
        key_out.color_1_view_format = xenos::ColorRenderTargetFormat(keys[2].format);
        break;
      case 3:
        key_out.color_2_view_format = xenos::ColorRenderTargetFormat(keys[3].format);
        break;
      case 4:
        key_out.color_3_view_format = xenos::ColorRenderTargetFormat(keys[4].format);
        break;
    }
    bound_bits_ |= 1u << i;
  }
  key_out.depth_and_color_used = bound_bits_;
  if (!used_bits) Count("draw_writes_nothing");
  return true;
}

uint64_t Fh1NativeExecutor::DrawRenderingId(uint32_t used_bits) const {
  // The scope is identified by the bound surfaces.
  uint64_t id = 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(used_bits & (1u << i))) continue;
    id = (id ^ (uint64_t(pending_keys_[i].Pack()) + i)) * 0x100000001B3ull;
  }
  id &= ~(uint64_t(1) << 63);
  return id | 1;
}

void Fh1NativeExecutor::BeginDrawRendering() {
  // Pending clears of the draw's surfaces are folded into its rendering.
  Surface* bound[1 + xenos::kMaxColorRenderTargets] = {};
  uint32_t clear_width = UINT32_MAX, clear_height = UINT32_MAX;
  bool clears = false;
  if (pending_clear_surfaces_) {
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (!(bound_bits_ & (1u << i))) continue;
      bound[i] = FindSurface(pending_keys_[i].Pack());
      if (!bound[i]) continue;
      clears |= !bound[i]->pending_clears.empty();
      clear_width = std::min(clear_width, bound[i]->width * scale_);
      clear_height = std::min(clear_height, bound[i]->height * scale_);
    }
    if (clears) {
      FoldPendingClears(bound, clear_width, clear_height, nullptr, nullptr, true);
    }
  }
  if (command_processor_.IsFh1RenderingOpen(DrawRenderingId(bound_bits_))) {
    // The draw continues the open rendering unless pending barriers end it;
    // then it begins anew below as before.
    command_processor_.SubmitBarriers(false);
    if (command_processor_.IsFh1RenderingOpen(DrawRenderingId(bound_bits_))) {
      const int32_t split = REXCVAR_GET(fh1_debug_rendering_split_draws);
      if (split <= 0 || ++rendering_draws_ < uint32_t(split)) {
        attachment_barriers_skipped_ = false;
        rendering_id_ = DrawRenderingId(bound_bits_);
        if (clears) FoldPendingClears(bound, clear_width, clear_height, nullptr, nullptr, false);
        return;
      }
      command_processor_.EndRenderPass();
      Count("debug_rendering_split");
      // Ordered after the previous instance like any rendering break.
      attachment_barriers_skipped_ = true;
    }
  }
  rendering_draws_ = 0;
  VkRenderingAttachmentInfo colors[xenos::kMaxColorRenderTargets] = {};
  VkRenderingAttachmentInfo depth = {};
  uint32_t color_count = 0;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(bound_bits_ & (1u << i))) continue;
    Surface* surface = FindSurface(pending_keys_[i].Pack());
    if (!surface) continue;
    VkRenderingAttachmentInfo& attachment = i ? colors[i - 1] : depth;
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = surface->view;
    attachment.imageLayout = surface->layout;
    attachment.resolveMode = VK_RESOLVE_MODE_NONE;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (i) color_count = i;
    width = std::min(width, surface->width * scale_);
    height = std::min(height, surface->height * scale_);
  }
  if (width == UINT32_MAX) {
    // No attachments: the surface pitch bounds the draw.
    const auto surface_info = register_file_.Get<reg::RB_SURFACE_INFO>();
    const uint32_t msaa = uint32_t(surface_info.msaa_samples);
    const uint32_t pitch_tiles = std::max(Fh1PitchTiles(surface_info.surface_pitch, msaa), 1u);
    const uint32_t msaa_x_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k4X));
    width = pitch_tiles * (xenos::kEdramTileWidthSamples >> msaa_x_log2) * scale_;
    height = SurfaceHeight(pitch_tiles, msaa) * scale_;
  }
  const uint64_t id = DrawRenderingId(bound_bits_);
  if (attachment_barriers_skipped_) {
    attachment_barriers_skipped_ = false;
    // Pending barriers (such as texture loads) end the rendering.
    command_processor_.SubmitBarriers(false);
    if (!command_processor_.IsFh1RenderingOpen(id)) {
      // Writes in another rendering instance are ordered by a barrier.
      Count("draw_attachment_barrier_late");
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        if (!(bound_bits_ & (1u << i))) continue;
        if (Surface* surface = FindSurface(pending_keys_[i].Pack())) {
          TransitionForAttachment(*surface);
        }
      }
    }
  }
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea.extent = {width, height};
  info.layerCount = 1;
  info.colorAttachmentCount = color_count;
  info.pColorAttachments = color_count ? colors : nullptr;
  if (bound_bits_ & 1) {
    info.pDepthAttachment = &depth;
    info.pStencilAttachment = &depth;
  }
  if (clears) FoldPendingClears(bound, clear_width, clear_height, colors, &depth, true);
  rendering_id_ = id;
  command_processor_.SubmitBarriersAndBeginFh1Rendering(info, id);
  if (clears) FoldPendingClears(bound, clear_width, clear_height, nullptr, nullptr, false);
}

void Fh1NativeExecutor::NativeDrawIssued(const Fh1DrawInfo& draw) {
  if (!initialized_) return;
  pending_targets_valid_ = false;
  if (draw.occlusion_query_active) Count("draw_in_occlusion_query");
  ++draws_;
}

void Fh1NativeExecutor::ClearSurfaceRect(Surface& surface, const Rect& guest_rect,
                                         uint32_t clear_value, uint32_t clear_value_lo) {
  if (REXCVAR_GET(fh1_debug_skip_clears)) return;
  const Rect rect = HostRect(guest_rect);
  VkClearAttachment clear = {};
  if (surface.key.is_depth) {
    const uint32_t depth_bits = (clear_value >> 8) & 0xFFFFFF;
    clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    clear.clearValue.depthStencil.depth =
        xenos::DepthRenderTargetFormat(surface.key.format) ==
                xenos::DepthRenderTargetFormat::kD24FS8
            ? xenos::Float20e4To32(depth_bits) * 0.5f
            : xenos::UNorm24To32(depth_bits);
    clear.clearValue.depthStencil.stencil = clear_value & 0xFF;
    surface.stencil_nonzero |= (clear_value & 0xFF) != 0;
  } else {
    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    float* color = clear.clearValue.color.float32;
    switch (xenos::ColorRenderTargetFormat(surface.key.format)) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        for (uint32_t i = 0; i < 4; ++i) {
          color[i] = float((clear_value >> (8 * i)) & 0xFF) / 255.0f;
        }
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        for (uint32_t i = 0; i < 3; ++i) {
          color[i] = float((clear_value >> (10 * i)) & 0x3FF) / 1023.0f;
        }
        color[3] = float(clear_value >> 30) / 3.0f;
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        for (uint32_t i = 0; i < 3; ++i) {
          color[i] = xenos::Float7e3To32((clear_value >> (10 * i)) & 0x3FF);
        }
        color[3] = float(clear_value >> 30) / 3.0f;
        break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        std::memcpy(&color[0], &clear_value, sizeof(float));
        break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        const bool is_float =
            surface.key.format == uint32_t(xenos::ColorRenderTargetFormat::k_16_16_FLOAT) ||
            surface.key.format == uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT);
        const uint32_t words[2] = {surface.key.Is64bpp() ? clear_value_lo : clear_value,
                                   clear_value};
        for (uint32_t i = 0; i < (surface.key.Is64bpp() ? 4u : 2u); ++i) {
          const uint16_t bits = uint16_t(words[i >> 1] >> (16 * (i & 1)));
          color[i] = is_float ? HalfToFloat(bits)
                              : std::max(float(int16_t(bits)) / 32767.0f, -1.0f);
        }
        break;
      }
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        std::memcpy(&color[0], &clear_value_lo, sizeof(float));
        std::memcpy(&color[1], &clear_value, sizeof(float));
        break;
      default:
        if (clear_value || clear_value_lo) return Skip("clear_format");
        break;
    }
  }
  VkClearRect vk_rect = {};
  vk_rect.rect = {{rect.left, rect.top},
                  {uint32_t(rect.right - rect.left), uint32_t(rect.bottom - rect.top)}};
  vk_rect.layerCount = 1;
  if (surface.pending_clears.empty()) ++pending_clear_surfaces_;
  surface.pending_clears.push_back({clear, vk_rect});
  if (!REXCVAR_GET(fh1_fold_clears)) FlushPendingClears(surface);
}

void Fh1NativeExecutor::FlushPendingClears(Surface& surface) {
  if (surface.pending_clears.empty()) return;
  const std::vector<Surface::PendingClear> clears = std::move(surface.pending_clears);
  surface.pending_clears.clear();
  --pending_clear_surfaces_;
  GpuTimer gpu_timer(*this, kGpuClears);
  command_processor_.Checkpoint(VulkanCommandProcessor::CheckpointKind::kClear);
  TransitionForAttachment(surface);
  BeginSurfaceRendering(surface, false);
  for (const Surface::PendingClear& pending : clears) {
    command_processor_.deferred_command_buffer().CmdVkClearAttachments(1, &pending.clear, 1,
                                                                       &pending.rect);
  }
  // Whatever renders into the surface next is ordered after the clears.
  TransitionForAttachment(surface);
  Count("clear_flushed");
}

void Fh1NativeExecutor::FoldPendingClears(Surface* const* bound, uint32_t width, uint32_t height,
                                          VkRenderingAttachmentInfo* colors,
                                          VkRenderingAttachmentInfo* depth, bool before_begin) {
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    Surface* surface = bound[i];
    if (!surface || surface->pending_clears.empty()) continue;
    auto& pending = surface->pending_clears;
    if (before_begin) {
      bool fits = true;
      for (const Surface::PendingClear& clear : pending) {
        const VkRect2D& r = clear.rect.rect;
        fits = fits && r.offset.x >= 0 && r.offset.y >= 0 &&
               uint32_t(r.offset.x) + r.extent.width <= width &&
               uint32_t(r.offset.y) + r.extent.height <= height;
      }
      if (!fits) {
        // Clears beyond the rendering's area keep a rendering of their own.
        FlushPendingClears(*surface);
        TransitionForAttachment(*surface);
        continue;
      }
      VkRenderingAttachmentInfo* attachment = i ? (colors ? &colors[i - 1] : nullptr) : depth;
      const VkRect2D& last = pending.back().rect.rect;
      if (attachment && !last.offset.x && !last.offset.y && last.extent.width == width &&
          last.extent.height == height) {
        // The last clear covers the whole rendering: it replaces the load.
        attachment->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment->clearValue = pending.back().clear.clearValue;
        pending.clear();
        --pending_clear_surfaces_;
        Count("clear_load_op");
      }
      continue;
    }
    for (const Surface::PendingClear& clear : pending) {
      VkClearAttachment attachment_clear = clear.clear;
      attachment_clear.colorAttachment = i ? i - 1 : 0;
      command_processor_.deferred_command_buffer().CmdVkClearAttachments(1, &attachment_clear, 1,
                                                                         &clear.rect);
      Count("clear_folded");
    }
    pending.clear();
    --pending_clear_surfaces_;
  }
}

bool Fh1NativeExecutor::ResolveToMemory(const SourceRect& source, const SurfaceKey& resolve_key,
                                        uint32_t sample_select, uint32_t dest_info,
                                        uint32_t dest_base, uint32_t dest_pitch,
                                        VkBuffer buffer, VkDeviceSize memory_offset,
                                        VkDeviceSize memory_range, bool unscaled_dest) {
  GpuTimer gpu_timer(*this, kGpuResolves);
  command_processor_.Checkpoint(VulkanCommandProcessor::CheckpointKind::kResolve);
  Surface& surface = *source.surface;
  const bool depth = surface.key.is_depth;
  const bool msaa = surface.samples > 1;
  VkPipeline pipeline = GetComputePipeline(false, SourceKind(depth, surface.key.format), msaa);
  if (!pipeline) {
    Skip("resolve_pipeline_create");
    return false;
  }
  VkDescriptorSet set = AllocateDescriptorSet(compute_set_layout_);
  if (!set) {
    Skip("resolve_descriptor");
    return false;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  TransitionForSampling(surface, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  command_processor_.SubmitBarriers(true);
  VkDescriptorImageInfo images[2] = {
      {VK_NULL_HANDLE, surface.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, surface.stencil_view ? surface.stencil_view : surface.sampled_view,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkDescriptorBufferInfo buffer_info = {buffer, memory_offset, memory_range};
  VkWriteDescriptorSet writes[3] = {};
  for (uint32_t w = 0; w < 3; ++w) {
    writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[w].dstSet = set;
    writes[w].descriptorCount = 1;
  }
  writes[0].dstBinding = kBindingSource;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[0].pImageInfo = &images[0];
  writes[1].dstBinding = kBindingStencil;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[1].pImageInfo = &images[1];
  writes[2].dstBinding = kBindingMemory;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo = &buffer_info;
  vulkan_device->functions().vkUpdateDescriptorSets(vulkan_device->device(), 3, writes, 0,
                                                    nullptr);
  const SurfaceKey& owner = surface.key;
  const uint32_t host_sample_mode = HostSampleMode(surface);
  const Rect rect = unscaled_dest ? source.rect : HostRect(source.rect);
  uint32_t constants[kComputeConstantCount];
  constants[0] = uint32_t(rect.left) | (uint32_t(rect.top) << 16);
  constants[1] = uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16);
  constants[2] = PackLayout(resolve_key.base_tiles, resolve_key.pitch_tiles, resolve_key.msaa,
                            resolve_key.Is64bpp(), resolve_key.is_depth, resolve_key.format);
  constants[3] = PackLayout(owner.base_tiles, owner.pitch_tiles, owner.msaa, owner.Is64bpp(),
                            owner.is_depth, owner.format) |
                 (host_sample_mode << 27);
  constants[4] = sample_select;
  constants[5] = dest_info | ((scale_ - 1) << 20) | (unscaled_dest ? 1u << 22 : 0u);
  constants[6] = dest_base;
  constants[7] = dest_pitch;
  auto& command_buffer = command_processor_.deferred_command_buffer();
  command_processor_.BindExternalComputePipeline(pipeline);
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline_layout_,
                                         0, 1, &set, 0, nullptr);
  command_buffer.CmdVkPushConstants(compute_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(constants), constants);
  command_processor_.BeginDebugLabel("fh1 resolve");
  command_buffer.CmdVkDispatch((uint32_t(rect.right - rect.left) + 7) / 8,
                               (uint32_t(rect.bottom - rect.top) + 7) / 8, 1);
  command_processor_.EndDebugLabel();
  return true;
}

bool Fh1NativeExecutor::PlanCopy(CopyPlan& plan) {
  plan = CopyPlan();
  const Fh1ResolveFlags flags{config_.depth_float24_round, config_.gamma_as_unorm16,
                              config_.fixed16_truncated};
  if (!Fh1PlanResolve(register_file_, memory_, flags, plan)) return false;
  if (plan.copy) {
    GetResolveSources(plan.resolve_key, plan.x0, plan.y0, plan.x1, plan.y1, plan.sources);
  }
  return true;
}

bool Fh1NativeExecutor::NativeResolve(uint32_t& written_address, uint32_t& written_length) {
  written_address = 0;
  written_length = 0;
  if (!initialized_) return false;
  if (!Resolve(&written_address, &written_length)) return false;
  // At scale the resolve reads one-off captures back itself.
  if (scale_ == 1 && written_length && IsOneOffResolve(written_address, written_length)) {
    QueueResolveReadback(written_address, written_length);
  }
  if (written_length && !REXCVAR_GET(fh1_resolve_dump_dir).empty() &&
      (!REXCVAR_GET(fh1_resolve_dump_frame) ||
       frame_ == uint64_t(REXCVAR_GET(fh1_resolve_dump_frame)))) {
    DumpResolveOutput(written_address, written_length);
  }
  return true;
}

void Fh1NativeExecutor::DumpResolveOutput(uint32_t address, uint32_t length) {
  // As the D3D12 command processor's fh1_resolve_dump_dir: every resolve's
  // output bytes, waiting for the GPU after each.
  static uint32_t sequence = 0;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkBuffer buffer;
  VkDeviceMemory memory;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, length, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, buffer, memory)) {
    return;
  }
  config_.memory->Use(VulkanSharedMemory::Usage::kRead);
  command_processor_.SubmitBarriers(true);
  const VkBufferCopy region = {address, 0, length};
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(config_.memory->buffer(), buffer,
                                                               1, &region);
  command_processor_.Fh1AwaitAllQueueOperations();
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
    VkMappedMemoryRange range = {};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
    char name[64];
    std::snprintf(name, sizeof(name), "%05u_%08X_%u.bin", sequence++, address, length);
    std::ofstream(std::filesystem::path(REXCVAR_GET(fh1_resolve_dump_dir)) / name,
                  std::ios::binary)
        .write(static_cast<const char*>(mapped), std::streamsize(length));
    dfn.vkUnmapMemory(device, memory);
  }
  dfn.vkDestroyBuffer(device, buffer, nullptr);
  dfn.vkFreeMemory(device, memory, nullptr);
}

bool Fh1NativeExecutor::IsOneOffResolve(uint32_t address, uint32_t length) {
  // As the D3D12 executor: the first frames of each run of resolves to a new,
  // large range are read back (car thumbnails the game saves).
  constexpr uint64_t kRunGapFrames = 4, kRunReadbackFrames = 2, kIdleFrames = 300,
                     kNewFrames = 120;
  constexpr uint32_t kMinLength = 256 * 1024;
  ResolveReadback& readback = resolve_readbacks_[(uint64_t(address) << 32) | length];
  if (!readback.last_used_frame || frame_ > readback.last_used_frame + kRunGapFrames) {
    if (!readback.last_used_frame || frame_ > readback.last_used_frame + kIdleFrames) {
      readback.new_since_frame = frame_;
    }
    readback.run_start_frame = frame_;
  }
  readback.last_used_frame = frame_;
  return length >= kMinLength && frame_ < readback.run_start_frame + kRunReadbackFrames &&
         frame_ < readback.new_since_frame + kNewFrames;
}

void Fh1NativeExecutor::QueueResolveReadback(uint32_t address, uint32_t length) {
  PendingReadback readback = {address, length, VK_NULL_HANDLE, VK_NULL_HANDLE};
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(), length, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, readback.buffer, readback.memory)) {
    return Skip("resolve_readback_buffer");
  }
  config_.memory->Use(VulkanSharedMemory::Usage::kRead);
  command_processor_.SubmitBarriers(true);
  const VkBufferCopy region = {address, 0, length};
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(config_.memory->buffer(),
                                                               readback.buffer, 1, &region);
  pending_readbacks_.push_back(readback);
  Count("resolve_readback_one_off");
}

void Fh1NativeExecutor::FlushResolveReadbacks() {
  if (pending_readbacks_.empty()) return;
  command_processor_.Fh1AwaitAllQueueOperations();
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // In order: a later resolve over the same bytes wins, as on the GPU.
  for (PendingReadback& pending : pending_readbacks_) {
    uint8_t* destination = memory_.TranslatePhysical(pending.address);
    void* mapped = nullptr;
    if (destination &&
        dfn.vkMapMemory(device, pending.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
      VkMappedMemoryRange range = {};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = pending.memory;
      range.size = VK_WHOLE_SIZE;
      dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
      std::memcpy(destination, mapped, pending.length);
      dfn.vkUnmapMemory(device, pending.memory);
    }
    dfn.vkDestroyBuffer(device, pending.buffer, nullptr);
    dfn.vkFreeMemory(device, pending.memory, nullptr);
  }
  pending_readbacks_.clear();
  Count("resolve_readback_flush");
}

namespace {
// Once per kind: which guest copy (source and destination formats, samples)
// the backend lacks, so a device report names it.
void LogSkippedResolve(const Fh1ResolvePlan& plan) {
  static std::mutex logged_mutex;
  static std::unordered_set<std::string> logged;
  std::lock_guard lock(logged_mutex);
  if (logged.insert(std::string(plan.skip) + ":" + plan.kind).second) {
    REXGPU_WARN("FH1 native executor (Vulkan) skipped a resolve: {} {} dest_info=0x{:08X}",
                plan.skip, plan.kind, plan.info.copy_dest_info.value);
  }
}
}  // namespace

bool Fh1NativeExecutor::Resolve(uint32_t* written_address, uint32_t* written_length) {
  const RegisterFile& regs = register_file_;
  CopyPlan plan;
  if (!PlanCopy(plan)) {
    if (plan.empty) {
      Count("resolve_empty");
      return true;
    }
    LogSkippedResolve(plan);
    Skip(plan.skip);
    return false;
  }
  bool succeeded = true;
  const int32_t x0 = plan.x0, y0 = plan.y0, x1 = plan.x1, y1 = plan.y1;
  const Rect rect = {x0, y0, x1, y1};
  const uint32_t msaa = plan.msaa;
  const uint32_t pitch_tiles = plan.pitch_tiles;
  const auto& resolve_info = plan.info;
  const auto& color_info = plan.color_info;
  const auto& depth_info = plan.depth_info;

  if (plan.skip) {
    LogSkippedResolve(plan);
    Skip(plan.skip);
    succeeded = false;
  } else if (plan.copy) {
    const uint32_t extent_start = resolve_info.copy_dest_extent_start;
    const uint32_t extent_length = resolve_info.copy_dest_extent_length;
    // The shader addresses guest memory from dest_base; the descriptor starts
    // at an aligned offset at or before both it and the written extent.
    const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
    const VkDeviceSize alignment =
        std::max<VkDeviceSize>(vulkan_device->properties().minStorageBufferOffsetAlignment, 4);
    const VkDeviceSize offset =
        VkDeviceSize(std::min(plan.dest_base, extent_start)) & ~(alignment - 1);
    const VkDeviceSize range = VkDeviceSize(extent_start) + extent_length - offset;
    // At scale the destination is the texture cache's scaled resolve buffer
    // from the scaled destination base, which the shader addresses relative
    // to (as the D3D12 executor's scaled resolve range).
    VkBuffer target = config_.memory->buffer();
    VkDeviceSize target_offset = offset, target_range = range;
    uint32_t dest_base = uint32_t(plan.dest_base - offset);
    if (scale_ > 1) {
      const uint32_t base = resolve_info.copy_dest_base;
      const uint32_t range_unscaled = extent_start - base + extent_length;
      uint64_t scaled_base, scaled_length, use_start, use_length;
      if (!config_.textures->GetScaledResolveRange(base, range_unscaled, 2, scaled_base,
                                                   scaled_length) ||
          !config_.textures->GetScaledResolveRange(extent_start, extent_length, 2, use_start,
                                                   use_length) ||
          !config_.textures->CommitScaledResolveRange(base, range_unscaled, 2)) {
        Skip("resolve_scaled_memory");
        return false;
      }
      config_.textures->UseScaledResolveBufferForWrite(use_start, use_length);
      target = config_.textures->scaled_resolve_buffer();
      target_offset = scaled_base;
      target_range = scaled_length;
      dest_base = plan.dest_base - base;
    } else {
      config_.memory->RequestRange(extent_start, extent_length);
      config_.memory->Use(VulkanSharedMemory::Usage::kComputeWrite,
                          std::make_pair(extent_start, extent_length));
    }
    bool complete = true;
    for (const SourceRect& source : plan.sources) {
      if (source.rect.left >= source.rect.right || source.rect.top >= source.rect.bottom) {
        continue;
      }
      if (!source.surface) {
        Skip(source.unowned ? "resolve_tiles_unowned" : "resolve_owner_missing");
        complete = false;
        continue;
      }
      const SurfaceKey& owner = source.surface->key;
      if (!owner.is_depth &&
          !Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(owner.format))) {
        Skip("resolve_owner_format");
        complete = false;
        continue;
      }
      if (owner.Pack() != (plan.copying_depth
                               ? plan.resolve_key.Pack()
                               : MakeColorKey(color_info.color_base, pitch_tiles, msaa,
                                              color_info.color_format)
                                     .Pack())) {
        Count("resolve_through_alias");
      }
      if (!ResolveToMemory(source, plan.resolve_key, plan.sample_select, plan.dest_info,
                           dest_base, plan.dest_pitch, target, target_offset, target_range)) {
        complete = false;
      }
    }
    // One-off captures the CPU reads also need the guest layout at scale:
    // resolved again unscaled into the guest memory copy, from each guest
    // pixel's first host pixel, and read back.
    if (scale_ > 1 && written_address && IsOneOffResolve(extent_start, extent_length)) {
      config_.memory->RequestRange(extent_start, extent_length);
      config_.memory->Use(VulkanSharedMemory::Usage::kComputeWrite,
                          std::make_pair(extent_start, extent_length));
      bool unscaled_complete = true;
      for (const SourceRect& source : plan.sources) {
        if (!source.surface || source.rect.left >= source.rect.right ||
            source.rect.top >= source.rect.bottom ||
            (!source.surface->key.is_depth &&
             !Fh1IsResolveColorFormatSupported(
                 xenos::ColorRenderTargetFormat(source.surface->key.format)))) {
          continue;
        }
        unscaled_complete &= ResolveToMemory(
            source, plan.resolve_key, plan.sample_select, plan.dest_info,
            uint32_t(plan.dest_base - offset), plan.dest_pitch, config_.memory->buffer(), offset,
            range, true);
      }
      if (unscaled_complete) QueueResolveReadback(extent_start, extent_length);
    }
    // Invalidates textures over the range (and marks it scaled at scale).
    config_.textures->MarkRangeAsResolved(extent_start, extent_length);
    if (written_address) *written_address = extent_start;
    if (written_length) *written_length = extent_length;
    if (complete) {
      ++resolves_;
      Count(plan.sources.size() > 1 ? "resolve_multi_owner" : "resolve_single_owner");
    }
    succeeded = complete;
  }

  // Clears target the surface configured at the original base and take
  // ownership of the cleared tiles.
  const uint32_t msaa_x_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k2X));
  auto clear = [&](const SurfaceKey& key, uint32_t value, uint32_t value_lo) {
    Surface* surface = GetOrCreateSurface(key);
    if (!surface) return Skip("resolve_clear_surface");
    const uint32_t is_64bpp = key.Is64bpp() ? 1 : 0;
    const uint32_t tile_width = (xenos::kEdramTileWidthSamples >> msaa_x_log2) >> is_64bpp;
    const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
    const uint32_t pitch = key.pitch_tiles << is_64bpp;
    const uint32_t column_first = uint32_t(x0) / tile_width;
    const uint32_t column_end = (uint32_t(x1) + tile_width - 1) / tile_width;
    const uint32_t inner_first = (uint32_t(x0) + tile_width - 1) / tile_width;
    const uint32_t inner_end = std::max(inner_first, uint32_t(x1) / tile_width);
    for (uint32_t row = uint32_t(y0) / tile_height;
         row < (uint32_t(y1) + tile_height - 1) / tile_height; ++row) {
      const uint32_t row_base = key.base_tiles + row * pitch;
      const bool row_covered =
          row * tile_height >= uint32_t(y0) && (row + 1) * tile_height <= uint32_t(y1);
      if (!row_covered || inner_first >= inner_end) {
        ClaimTiles(row_base + column_first, column_end - column_first, key.Pack());
        continue;
      }
      if (column_first < inner_first) {
        ClaimTiles(row_base + column_first, inner_first - column_first, key.Pack());
      }
      ClaimTiles(row_base + inner_first, inner_end - inner_first, key.Pack(), false);
      if (inner_end < column_end) {
        ClaimTiles(row_base + inner_end, column_end - inner_end, key.Pack());
      }
    }
    for (uint32_t row = uint32_t(y0) / tile_height;
         row < (uint32_t(y1) + tile_height - 1) / tile_height; ++row) {
      const uint32_t row_base = key.base_tiles + row * pitch;
      const bool row_covered =
          row * tile_height >= uint32_t(y0) && (row + 1) * tile_height <= uint32_t(y1);
      if (!key.is_depth || (value & 0xFF)) {
        MarkTileStencil(row_base + column_first, column_end - column_first, true);
      } else if (row_covered && inner_first < inner_end) {
        MarkTileStencil(row_base + inner_first, inner_end - inner_first, false);
      }
    }
    FlushTransfers();
    ClearSurfaceRect(*surface, rect, value, value_lo);
    Count(key.is_depth ? "clear_depth" : "clear_color");
  };
  if (resolve_info.IsClearingColor()) {
    clear(MakeColorKey(color_info.color_base, pitch_tiles, msaa, color_info.color_format),
          regs[XE_GPU_REG_RB_COLOR_CLEAR], regs[XE_GPU_REG_RB_COLOR_CLEAR_LO]);
  }
  if (resolve_info.IsClearingDepth()) {
    clear(MakeDepthKey(depth_info.depth_base, pitch_tiles, msaa, depth_info.depth_format),
          regs[XE_GPU_REG_RB_DEPTH_CLEAR], 0);
  }
  return succeeded;
}

void Fh1NativeExecutor::OnSwap(uint64_t frame) {
  if (!initialized_) return;
  GpuEndFrame();
  GpuDrain();
  frame_ = frame;
  if (frame % 600 == 0) LogStats(frame);
}

void Fh1NativeExecutor::LogStats(uint64_t frame) {
  REXGPU_INFO(
      "FH1 native executor (Vulkan) frame={} draws={} resolves={} surfaces={} skips={{{}}} "
      "stats={{{}}}",
      frame, draws_, resolves_, surfaces_.size(), counters_.FormatSkips(),
      counters_.FormatStats());
  if (gpu_frames_) {
    const double tick_ns = command_processor_.GetVulkanDevice()->properties().timestampPeriod;
    const double scale = tick_ns / 1e6 / double(gpu_frames_);
    REXGPU_INFO(
        "FH1 native executor (Vulkan) gpu ms/frame over {} frames: frame {:.3f} transfers "
        "{:.3f} resolves {:.3f} clears {:.3f} texture_reloads {:.3f} texture_loads {:.3f}",
        gpu_frames_, gpu_ticks_[kGpuFrame] * scale, gpu_ticks_[kGpuTransfers] * scale,
        gpu_ticks_[kGpuResolves] * scale, gpu_ticks_[kGpuClears] * scale,
        gpu_ticks_[kGpuTextureReloads] * scale, gpu_ticks_[kGpuTextureLoads] * scale);
    gpu_ticks_ = {};
    gpu_frames_ = 0;
  }
  if (!transfer_volume_.empty()) {
    std::vector<std::pair<uint64_t, std::string>> volume;
    for (const auto& [pair, tiles] : transfer_volume_) volume.emplace_back(tiles, pair);
    std::sort(volume.begin(), volume.end(), std::greater<>());
    std::string top;
    for (size_t i = 0; i < volume.size() && i < 8; ++i) {
      top += fmt::format("{}{}={}", i ? " " : "", volume[i].second, volume[i].first);
    }
    REXGPU_INFO("FH1 native executor (Vulkan) transfer tile-passes: {}", top);
    transfer_volume_.clear();
  }
  const uint64_t window_frames = frame >= 600 ? 600 : std::max<uint64_t>(frame, 1);
  const auto causes = command_processor_.TakeFh1RenderingCauses();
  const double per_frame = 1.0 / double(window_frames);
  REXGPU_INFO(
      "FH1 native executor (Vulkan) per frame over {} frames: renderings {:.1f} barrier "
      "batches {:.1f}; renderings begun after barriers on the same surfaces {:.1f}, barriers "
      "and new surfaces {:.1f}, new surfaces {:.1f}, other work {:.1f}",
      window_frames, double(command_processor_.TakeRenderingBeginCount()) * per_frame,
      double(command_processor_.TakeBarrierBatchCount()) * per_frame,
      double(causes[VulkanCommandProcessor::kFh1RenderingBarriersSameSurfaces]) * per_frame,
      double(causes[VulkanCommandProcessor::kFh1RenderingBarriersNewSurfaces]) * per_frame,
      double(causes[VulkanCommandProcessor::kFh1RenderingNewSurfaces]) * per_frame,
      double(causes[VulkanCommandProcessor::kFh1RenderingEndedElsewhere]) * per_frame);
  std::vector<std::pair<uint64_t, std::string>> enders;
  for (auto& [name, count] : command_processor_.TakeFh1RenderingEnders()) {
    enders.emplace_back(count, name);
  }
  std::sort(enders.begin(), enders.end(), std::greater<>());
  std::string top;
  for (size_t i = 0; i < enders.size() && i < 6; ++i) {
    top += fmt::format("{}{:.1f} {}", i ? "; " : "", double(enders[i].first) * per_frame,
                       enders[i].second);
  }
  if (!top.empty()) {
    REXGPU_INFO("FH1 native executor (Vulkan) renderings ended by other work, per frame: {}", top);
  }
}

uint32_t Fh1NativeExecutor::GpuBegin() {
  if (gpu_query_pool_ == VK_NULL_HANDLE) return UINT32_MAX;
  GpuProfileSlot& slot = gpu_slots_[gpu_slot_];
  if (slot.pending) return UINT32_MAX;  // The previous results are not read yet.
  auto& command_buffer = command_processor_.deferred_command_buffer();
  const uint32_t base = gpu_slot_ * kGpuProfileQueries;
  if (!slot.used) {
    // Queries are reset outside rendering; the next draw begins it again.
    command_processor_.EndRenderPass();
    command_buffer.CmdVkResetQueryPool(gpu_query_pool_, base, kGpuProfileQueries);
    // Query 0: where the frame's measured work starts.
    command_buffer.CmdVkWriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_query_pool_,
                                       base);
    slot.used = 1;
  }
  if (slot.used + 2 > kGpuProfileQueries) return UINT32_MAX;
  const uint32_t query = slot.used++;
  command_buffer.CmdVkWriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_query_pool_,
                                     base + query);
  return query;
}

void Fh1NativeExecutor::GpuEnd(GpuPhase phase, uint32_t begin) {
  if (begin == UINT32_MAX) return;
  GpuProfileSlot& slot = gpu_slots_[gpu_slot_];
  if (slot.used >= kGpuProfileQueries) return;
  const uint32_t query = slot.used++;
  command_processor_.deferred_command_buffer().CmdVkWriteTimestamp(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_query_pool_,
      gpu_slot_ * kGpuProfileQueries + query);
  slot.spans.push_back({uint32_t(phase), begin, query});
}

void Fh1NativeExecutor::GpuEndFrame() {
  if (gpu_query_pool_ == VK_NULL_HANDLE) return;
  GpuProfileSlot& slot = gpu_slots_[gpu_slot_];
  if (slot.pending || !slot.used || slot.used >= kGpuProfileQueries) return;
  const uint32_t query = slot.used++;
  command_processor_.deferred_command_buffer().CmdVkWriteTimestamp(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_query_pool_,
      gpu_slot_ * kGpuProfileQueries + query);
  slot.spans.push_back({uint32_t(kGpuFrame), 0, query});
  slot.submission = command_processor_.GetCurrentSubmission();
  slot.pending = true;
  gpu_slot_ = (gpu_slot_ + 1) % kGpuProfileSlots;
}

void Fh1NativeExecutor::GpuDrain() {
  if (gpu_query_pool_ == VK_NULL_HANDLE) return;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  std::vector<uint64_t> ticks;
  for (uint32_t index = 0; index < kGpuProfileSlots; ++index) {
    GpuProfileSlot& slot = gpu_slots_[index];
    if (!slot.pending || slot.submission > completed) continue;
    ticks.resize(slot.used);
    if (vulkan_device->functions().vkGetQueryPoolResults(
            vulkan_device->device(), gpu_query_pool_, index * kGpuProfileQueries, slot.used,
            ticks.size() * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (const auto& [phase, begin, end] : slot.spans) {
        if (ticks[end] > ticks[begin]) gpu_ticks_[phase] += ticks[end] - ticks[begin];
      }
      ++gpu_frames_;
    }
    slot = GpuProfileSlot();
  }
}

}  // namespace rex::graphics::vulkan
