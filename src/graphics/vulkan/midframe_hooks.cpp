/**
 * @file        graphics/vulkan/midframe_hooks.cpp
 *
 * @brief       Host passes in the middle of a frame (SR-11): a trigger before
 *              the first draw with a given guest pixel shader.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/vulkan/command_processor.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(vulkan_midframe_hook_pixel_shaders, "", "GPU",
                      "Comma-separated guest pixel shader hashes (16 hex digits): before the "
                      "first draw with each in a frame, end rendering and run the mid-frame "
                      "host passes (SR-11). Empty: no trigger. The first draws that fire are "
                      "logged")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

void VulkanCommandProcessor::AddMidframeHook(std::string name, MidframeHook hook) {
  midframe_hooks_.emplace_back(std::move(name), std::move(hook));
}

void VulkanCommandProcessor::MaybeRunMidframeHooks(const VulkanShader* pixel_shader) {
  const std::string list = REXCVAR_GET(vulkan_midframe_hook_pixel_shaders);
  if (list.empty())
    return;
  if (list != midframe_hook_list_) {
    midframe_hook_list_ = list;
    midframe_hook_hashes_.clear();
    size_t start = 0;
    while (start < list.size()) {
      const size_t end = std::min(list.find(',', start), list.size());
      std::string item = list.substr(start, end - start);
      start = end + 1;
      item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return c == ' '; }),
                 item.end());
      if (item.size() != 16)
        continue;
      char* rest = nullptr;
      const uint64_t hash = std::strtoull(item.c_str(), &rest, 16);
      if (rest && *rest == '\0')
        midframe_hook_hashes_.push_back(hash);
    }
    midframe_hook_fired_.clear();
  }
  if (midframe_hook_frame_ != frame_current_) {
    midframe_hook_frame_ = frame_current_;
    midframe_hook_fired_.clear();
    midframe_hook_draws_ = 0;
  }
  ++midframe_hook_draws_;
  if (!pixel_shader)
    return;
  const uint64_t hash = pixel_shader->ucode_data_hash();
  if (std::find(midframe_hook_hashes_.begin(), midframe_hook_hashes_.end(), hash) ==
          midframe_hook_hashes_.end() ||
      std::find(midframe_hook_fired_.begin(), midframe_hook_fired_.end(), hash) !=
          midframe_hook_fired_.end()) {
    return;
  }
  midframe_hook_fired_.push_back(hash);
  EndRenderPass();
  const MidframeHookContext context = {hash, frame_current_, midframe_hook_draws_};
  for (const auto& [name, hook] : midframe_hooks_)
    hook(context);
  if (midframe_hook_logged_ < 8) {
    ++midframe_hook_logged_;
    REXGPU_INFO("Mid-frame hook: {:016X} at draw {} of frame {}, {} host passes", hash,
                midframe_hook_draws_, frame_current_, midframe_hooks_.size());
  }
}

}  // namespace rex::graphics::vulkan
