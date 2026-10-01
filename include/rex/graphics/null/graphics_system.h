/**
 * @file        graphics/null/graphics_system.h
 * @brief       Null GPU backend: consumes the ring buffer, draws nothing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::null {

// Runs the guest's GPU side without a device or a window: the base command
// processor executes every CPU-visible packet (register and memory writes,
// fences, waits, interrupts, swaps) and the vblank thread ticks, while draws,
// resolves and shaders are dropped. Used to boot the title where no renderer
// exists yet (a new platform, CI) and as a renderer-free control for guest
// correctness. Requested with gpu_backend=null; never picked by "any".
class NullGraphicsSystem : public GraphicsSystem {
 public:
  NullGraphicsSystem();
  ~NullGraphicsSystem() override;

  std::string name() const override;

  // No provider and no presenter: the window stays empty.
  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext* app_context) override;
  bool has_presentation() const override { return presentation_set_up_; }
  uint32_t draw_resolution_scale() const override { return 1; }

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;

 private:
  bool presentation_set_up_ = false;
};

class NullCommandProcessor : public CommandProcessor {
 public:
  NullCommandProcessor(NullGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NullCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  NullGraphicsSystem* null_graphics_system_;
  uint64_t presented_output_frames_ = 0;
};

}  // namespace rex::graphics::null
