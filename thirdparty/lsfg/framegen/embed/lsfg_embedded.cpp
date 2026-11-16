// ShiftGlue: the per-frame flow of lsfg-vk 1.0.0's v3.1 context.cpp (MIT,
// see ../LICENSE.md), recorded into one host command buffer.

#include "lsfg_embedded.h"

#include "volk.h"

#include "common/exception.hpp"
#include "common/utils.hpp"
#include "core/commandbuffer.hpp"
#include "core/commandpool.hpp"
#include "core/descriptorpool.hpp"
#include "core/device.hpp"
#include "core/image.hpp"
#include "pool/resourcepool.hpp"
#include "pool/shaderpool.hpp"

#include "v3_1/shaders/alpha.hpp"
#include "v3_1/shaders/beta.hpp"
#include "v3_1/shaders/delta.hpp"
#include "v3_1/shaders/gamma.hpp"
#include "v3_1/shaders/generate.hpp"
#include "v3_1/shaders/mipmaps.hpp"
#include "v3_1p/shaders/alpha.hpp"
#include "v3_1p/shaders/beta.hpp"
#include "v3_1p/shaders/delta.hpp"
#include "v3_1p/shaders/gamma.hpp"
#include "v3_1p/shaders/generate.hpp"
#include "v3_1p/shaders/mipmaps.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>

namespace lsfg {

namespace {

// History the passes keep: alpha's outputs rotate over three frames.
constexpr uint64_t kWarmupFrames = 3;

template <typename Mipmaps, typename Alpha, typename Beta, typename Gamma, typename Delta,
          typename Generate>
class Chain {
 public:
  Chain(LSFG::Vulkan& vk, VkExtent2D extent) {
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    in_0_ = LSFG::Core::Image(vk.device, extent, VK_FORMAT_R8G8B8A8_UNORM, kUsage);
    in_1_ = LSFG::Core::Image(vk.device, extent, VK_FORMAT_R8G8B8A8_UNORM, kUsage);
    mipmaps_ = Mipmaps(vk, in_0_, in_1_);
    for (size_t i = 0; i < 7; i++) {
      alpha_.at(i) = Alpha(vk, mipmaps_.getOutImages().at(i));
    }
    beta_ = Beta(vk, alpha_.at(0).getOutImages());
    for (size_t i = 0; i < 7; i++) {
      gamma_.at(i) = Gamma(
          vk, alpha_.at(6 - i).getOutImages(), beta_.getOutImages().at(std::min<size_t>(6 - i, 5)),
          (i == 0) ? std::nullopt : std::make_optional(gamma_.at(i - 1).getOutImage()));
      if (i < 4) {
        continue;
      }
      delta_.at(i - 4) =
          Delta(vk, alpha_.at(6 - i).getOutImages(), beta_.getOutImages().at(6 - i),
                (i == 4) ? std::nullopt : std::make_optional(gamma_.at(i - 1).getOutImage()),
                (i == 4) ? std::nullopt : std::make_optional(delta_.at(i - 5).getOutImage1()),
                (i == 4) ? std::nullopt : std::make_optional(delta_.at(i - 5).getOutImage2()));
    }
    generate_ = Generate(vk, in_0_, in_1_, gamma_.at(6).getOutImage(),
                         delta_.at(2).getOutImage1(), delta_.at(2).getOutImage2(),
                         VK_FORMAT_R8G8B8A8_UNORM);
  }

  bool Record(VkCommandBuffer command_buffer, VkImage present_color, VkImage output,
              bool generate) {
    const LSFG::Core::CommandBuffer buffer(command_buffer);
    // inImg_0 is the newest frame when the index is even.
    LSFG::Core::Image& next = (frame_ % 2 == 0) ? in_0_ : in_1_;
    const VkExtent2D extent = next.getExtent();
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageCopy copy = {};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {extent.width, extent.height, 1};

    // The real frame into the history.
    {
      VkImageMemoryBarrier barriers[2] = {};
      barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[0].image = present_color;
      barriers[0].subresourceRange = range;
      barriers[1] = barriers[0];
      barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barriers[1].image = next.handle();
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                           barriers);
      vkCmdCopyImage(command_buffer, present_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     next.handle(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
      barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      // The passes' own barriers make the copy visible (write to read).
      barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                           barriers);
      next.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    }

    // lsfg-vk's first command buffer: the new frame's pyramid and features.
    mipmaps_.Dispatch(buffer, frame_);
    for (size_t i = 0; i < 7; i++) {
      alpha_.at(6 - i).Dispatch(buffer, frame_);
    }
    beta_.Dispatch(buffer, frame_);

    const bool write = generate && frame_ + 1 >= kWarmupFrames;
    if (write) {
      // Its second: the flow and the frame halfway between (one pass).
      for (size_t i = 0; i < 7; i++) {
        gamma_.at(i).Dispatch(buffer, frame_, 0);
        if (i >= 4) {
          delta_.at(i - 4).Dispatch(buffer, frame_, 0);
        }
      }
      generate_.Dispatch(buffer, frame_, 0);

      LSFG::Core::Image& generated = generate_.getOutImages().at(0);
      VkImageMemoryBarrier barrier = {};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = generated.handle();
      barrier.subresourceRange = range;
      VkImageMemoryBarrier output_barrier = barrier;
      output_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      output_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      output_barrier.image = output;
      const VkImageMemoryBarrier before[2] = {barrier, output_barrier};
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);
      vkCmdCopyImage(command_buffer, generated.handle(), VK_IMAGE_LAYOUT_GENERAL, output,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
      // The generated image is written again next frame; the output is read
      // by the presenter after a compute-write barrier of its own.
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      output_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      output_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      const VkImageMemoryBarrier after[2] = {barrier, output_barrier};
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                           after);
    }
    ++frame_;
    return write;
  }

  VkExtent2D extent() const { return in_0_.getExtent(); }

 private:
  LSFG::Core::Image in_0_;
  LSFG::Core::Image in_1_;
  uint64_t frame_ = 0;
  Mipmaps mipmaps_;
  std::array<Alpha, 7> alpha_;
  Beta beta_;
  std::array<Gamma, 7> gamma_;
  std::array<Delta, 3> delta_;
  Generate generate_;
};

namespace s31 = LSFG_3_1::Shaders;
namespace s31p = LSFG_3_1P::Shaders;
using Chain31 = Chain<s31::Mipmaps, s31::Alpha, s31::Beta, s31::Gamma, s31::Delta, s31::Generate>;
using Chain31p =
    Chain<s31p::Mipmaps, s31p::Alpha, s31p::Beta, s31p::Gamma, s31p::Delta, s31p::Generate>;

class EmbeddedImpl final : public Embedded {
 public:
  EmbeddedImpl(LSFG::Vulkan vk, bool performance) : vk_(std::move(vk)), performance_(performance) {}

  bool Resize(uint32_t width, uint32_t height, std::string* error) override {
    const VkExtent2D extent = {width, height};
    const std::optional<VkExtent2D> current =
        performance_ ? (chain_p_ ? std::make_optional(chain_p_->extent()) : std::nullopt)
                     : (chain_ ? std::make_optional(chain_->extent()) : std::nullopt);
    if (current && current->width == width && current->height == height) {
      return true;
    }
    chain_.reset();
    chain_p_.reset();
    try {
      if (performance_) {
        chain_p_.emplace(vk_, extent);
      } else {
        chain_.emplace(vk_, extent);
      }
    } catch (const std::exception& exception) {
      chain_.reset();
      chain_p_.reset();
      if (error) {
        *error = exception.what();
      }
      return false;
    }
    return true;
  }

  bool Record(VkCommandBuffer command_buffer, VkImage present_color, VkImage output,
              bool generate) override {
    if (chain_p_) {
      return chain_p_->Record(command_buffer, present_color, output, generate);
    }
    if (chain_) {
      return chain_->Record(command_buffer, present_color, output, generate);
    }
    return false;
  }

  ~EmbeddedImpl() override {
    chain_.reset();
    chain_p_.reset();
  }

 private:
  LSFG::Vulkan vk_;
  bool performance_;
  std::optional<Chain31> chain_;
  std::optional<Chain31p> chain_p_;
};

}  // namespace

std::unique_ptr<Embedded> Embedded::Create(const EmbeddedDevice& device, ShaderSource shaders,
                                           const EmbeddedSettings& settings,
                                           std::string* error) {
  if (!LSFG::LoadVkTable(device.get_instance_proc_addr, device.instance,
                         device.get_device_proc_addr, device.device)) {
    if (error) {
      *error = "a Vulkan entry point is missing";
    }
    return nullptr;
  }
  try {
    const float flow_scale = std::clamp(settings.flow_scale, 0.25f, 1.0f);
    LSFG::Vulkan vk{
        .device = LSFG::Core::Device(device.device, device.physical_device, device.queue_family,
                                     VK_NULL_HANDLE),
        .generationCount = 1,
        // lsfg-vk passes the inverse of the player's flow scale.
        .flowScale = 1.0f / flow_scale,
        .isHdr = false,
    };
    vk.commandPool = LSFG::Core::CommandPool(vk.device);
    vk.descriptorPool = LSFG::Core::DescriptorPool(vk.device);
    vk.resources = LSFG::Pool::ResourcePool(vk.isHdr, vk.flowScale);
    vk.shaders = LSFG::Pool::ShaderPool(std::move(shaders));
    return std::make_unique<EmbeddedImpl>(std::move(vk), settings.performance);
  } catch (const std::exception& exception) {
    if (error) {
      *error = exception.what();
    }
    return nullptr;
  }
}

}  // namespace lsfg
