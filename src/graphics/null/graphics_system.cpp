/**
 * @file        graphics/null/graphics_system.cpp
 * @brief       Null GPU backend: consumes the ring buffer, draws nothing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/null/graphics_system.h>

#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/system/interfaces/graphics.h>

namespace rex::graphics::null {

NullGraphicsSystem::NullGraphicsSystem() = default;

NullGraphicsSystem::~NullGraphicsSystem() = default;

std::string NullGraphicsSystem::name() const {
  return "Null";
}

X_STATUS NullGraphicsSystem::SetupPresentation(::rex::ui::WindowedAppContext* app_context) {
  app_context_ = app_context;
  presentation_set_up_ = true;
  REXGPU_INFO("Null GPU backend: guest GPU packets run, nothing is drawn or presented");
  return X_STATUS_SUCCESS;
}

void NullGraphicsSystem::CreateProvider(bool with_presentation) {
  (void)with_presentation;
}

std::unique_ptr<CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, kernel_state_);
}

NullCommandProcessor::NullCommandProcessor(NullGraphicsSystem* graphics_system,
                                           system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), null_graphics_system_(graphics_system) {}

NullCommandProcessor::~NullCommandProcessor() = default;

bool NullCommandProcessor::SetupContext() {
  return true;
}

void NullCommandProcessor::ShutdownContext() {}

Shader* NullCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                         const uint32_t* host_address, uint32_t dword_count) {
  (void)shader_type;
  (void)guest_address;
  (void)host_address;
  (void)dword_count;
  return nullptr;
}

bool NullCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                     IndexBufferInfo* index_buffer_info,
                                     bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  return true;
}

bool NullCommandProcessor::IssueCopy() {
  return true;
}

void NullCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  // Hosts count output frames here (render tests pace and stop on them), as
  // they do on the drawing backends' presents.
  const auto& renderer = null_graphics_system_->native_guest_output_renderer();
  if (renderer.IsRegistered()) {
    system::NativeGuestOutputRenderContext context;
    context.backend = system::NativeGuestOutputBackend::kNull;
    context.phase = system::NativeGuestOutputPhase::kPresented;
    context.guest_output_width = frontbuffer_width;
    context.guest_output_height = frontbuffer_height;
    context.display_width = frontbuffer_width;
    context.display_height = frontbuffer_height;
    context.frame_sequence = presented_output_frames_;
    context.presenter = system::NativeGuestOutputPresenter::kNull;
    renderer.Invoke(context);
  }
  ++presented_output_frames_;
}

}  // namespace rex::graphics::null
