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

#pragma once

#include "rex/system/function_dispatcher.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/graphics/register_file.h>
#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xthread.h>
#include <rex/thread/mutex.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>

// Forward declarations
namespace rex {
class Memory;
namespace stream {
class ByteStream;
}  // namespace stream
}  // namespace rex

namespace rex::ui {
class WindowedAppContext;
}  // namespace rex::ui

namespace rex::graphics {

class CommandProcessor;

class GraphicsSystem : public system::IGraphicsSystem {
 public:
  virtual ~GraphicsSystem();

  virtual std::string name() const = 0;

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  system::KernelState* kernel_state() const { return kernel_state_; }
  ::rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  ::rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  void Shutdown() override;

  // May be called from any thread any number of times, even during recovery
  // from a device loss.
  void OnHostGpuLossFromAnyThread(bool is_responsible);

  RegisterFile* register_file() { return &register_file_; }
  CommandProcessor* command_processor() const { return command_processor_.get(); }
  void SetDrawObserver(system::GraphicsDrawObserver observer) override {
    draw_observer_.store(observer, std::memory_order_release);
  }
  system::GraphicsDrawObserver draw_observer() const {
    return draw_observer_.load(std::memory_order_acquire);
  }
  void SetShaderConstantWriteObserver(
      system::GraphicsShaderConstantWriteObserver observer) override {
    shader_constant_write_observer_.store(observer,
                                          std::memory_order_release);
  }
  system::GraphicsShaderConstantWriteObserver
  shader_constant_write_observer() const {
    return shader_constant_write_observer_.load(std::memory_order_acquire);
  }
  void SetIndirectBufferObserver(
      system::GraphicsIndirectBufferObserver observer) override {
    indirect_buffer_observer_.store(observer, std::memory_order_release);
  }
  system::GraphicsIndirectBufferObserver indirect_buffer_observer() const {
    return indirect_buffer_observer_.load(std::memory_order_acquire);
  }
  void SetCopyObserver(system::GraphicsCopyObserver observer) override {
    copy_observer_.store(observer, std::memory_order_release);
  }
  system::GraphicsCopyObserver copy_observer() const {
    return copy_observer_.load(std::memory_order_acquire);
  }
  void SetShaderTranslationObserver(
      system::GraphicsShaderTranslationObserver observer) override {
    shader_translation_observer_.store(observer, std::memory_order_release);
  }
  system::GraphicsShaderTranslationObserver shader_translation_observer() const {
    return shader_translation_observer_.load(std::memory_order_acquire);
  }
  void SetPreparedDrawObserver(system::GraphicsPreparedDrawObserver observer) override {
    prepared_draw_observer_.store(observer, std::memory_order_release);
  }
  system::GraphicsPreparedDrawObserver prepared_draw_observer() const {
    return prepared_draw_observer_.load(std::memory_order_acquire);
  }
  void SetDrawOutcomeObserver(
      system::GraphicsDrawOutcomeObserver observer) override {
    draw_outcome_observer_.store(observer, std::memory_order_release);
  }
  system::GraphicsDrawOutcomeObserver draw_outcome_observer() const {
    return draw_outcome_observer_.load(std::memory_order_acquire);
  }
  void SetIsolatedDrawRequestObserver(
      system::GraphicsIsolatedDrawRequestObserver observer) override {
    isolated_draw_request_observer_.store(observer,
                                          std::memory_order_release);
  }
  system::GraphicsIsolatedDrawRequestObserver isolated_draw_request_observer()
      const {
    return isolated_draw_request_observer_.load(std::memory_order_acquire);
  }
  void SetNativeTextureSetObserver(
      system::GraphicsNativeTextureSetObserver observer) override {
    native_texture_set_observer_.store(observer, std::memory_order_release);
  }
  system::GraphicsNativeTextureSetObserver native_texture_set_observer() const {
    return native_texture_set_observer_.load(std::memory_order_acquire);
  }
  void SetNativeResolveObserver(
      system::GraphicsNativeResolveObserver observer) override {
    native_resolve_observer_.store(observer, std::memory_order_release);
  }
  system::GraphicsNativeResolveObserver native_resolve_observer() const {
    return native_resolve_observer_.load(std::memory_order_acquire);
  }
  void SetNativeGuestOutputRenderer(
      system::NativeGuestOutputRenderer renderer) override {
    native_guest_output_renderer_.Set(renderer);
  }
  bool HasNativeGuestOutputRenderer() const override {
    return native_guest_output_renderer_.IsRegistered();
  }
  const system::NativeGuestOutputRendererRegistration&
  native_guest_output_renderer() const {
    return native_guest_output_renderer_;
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(::rex::stream::ByteStream* stream);
  bool Restore(::rex::stream::ByteStream* stream);

 protected:
  GraphicsSystem();

  // Backends build their provider here. Called lazily from either setup
  // entry point; with_presentation is false only on headless guest-GPU paths.
  virtual void CreateProvider(bool with_presentation) = 0;

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static uint32_t ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                 uint32_t value);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  void MarkVblank();

  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  system::KernelState* kernel_state_ = nullptr;
  ::rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<::rex::ui::GraphicsProvider> provider_;
  bool provider_supports_presentation_ = false;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;

  std::atomic<bool> vsync_worker_running_;
  system::object_ref<system::XHostThread> vsync_worker_thread_;

  RegisterFile register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;

  bool paused_ = false;

 private:
  std::unique_ptr<::rex::ui::Presenter> presenter_;

  std::atomic<system::GraphicsDrawObserver> draw_observer_{nullptr};
  std::atomic<system::GraphicsShaderConstantWriteObserver>
      shader_constant_write_observer_{nullptr};
  std::atomic<system::GraphicsIndirectBufferObserver>
      indirect_buffer_observer_{nullptr};
  std::atomic<system::GraphicsCopyObserver> copy_observer_{nullptr};
  std::atomic<system::GraphicsShaderTranslationObserver>
      shader_translation_observer_{nullptr};
  std::atomic<system::GraphicsPreparedDrawObserver> prepared_draw_observer_{
      nullptr};
  std::atomic<system::GraphicsDrawOutcomeObserver> draw_outcome_observer_{
      nullptr};
  std::atomic<system::GraphicsIsolatedDrawRequestObserver>
      isolated_draw_request_observer_{nullptr};
  std::atomic<system::GraphicsNativeTextureSetObserver>
      native_texture_set_observer_{nullptr};
  std::atomic<system::GraphicsNativeResolveObserver> native_resolve_observer_{
      nullptr};
  system::NativeGuestOutputRendererRegistration native_guest_output_renderer_;

  std::atomic_flag host_gpu_loss_reported_;
};

}  // namespace rex::graphics
