/**
 * @file        system/interfaces/graphics.h
 * @brief       Abstract graphics system interface for dependency injection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>

#include <rex/system/xtypes.h>

// Forward declarations
namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::ui {
class GraphicsProvider;
class Presenter;
class WindowedAppContext;
}  // namespace rex::ui
namespace rex::system {

struct NativeGuestOutputRenderContext;
class KernelState;
}

namespace rex::system {

enum class NativeGuestOutputBackend : uint32_t {
  kUnsupported = 0,
  kD3D12 = 1,
  kVulkan = 2,
};

enum class NativeGuestOutputPhase : uint32_t {
  // The guest output has been presented (the only notification).
  kPresented = 1,
};

// Which renderer produced the guest output of a kPresented notification.
enum class NativeGuestOutputPresenter : uint32_t {
  // The FH1 native executor, the only renderer.
  kNativeExecutor = 2,
};

struct NativeGuestOutputRenderContext {
  NativeGuestOutputBackend backend = NativeGuestOutputBackend::kUnsupported;
  NativeGuestOutputPhase phase = NativeGuestOutputPhase::kPresented;
  uint32_t guest_output_width = 0;
  uint32_t guest_output_height = 0;
  uint32_t display_width = 0;
  uint32_t display_height = 0;
  uint32_t output_format = 0;
  void* device = nullptr;
  void* command_context = nullptr;
  void* guest_output = nullptr;
  uint32_t guest_output_state = 0;
  uint64_t submission = 0;
  uint64_t completed_submission = 0;
  uint64_t frame_sequence = 0;
  bool use_pwl_gamma_ramp = false;
  NativeGuestOutputPresenter presenter = NativeGuestOutputPresenter::kNativeExecutor;
};

// Notified after the guest output has been presented. The callback observes
// the presented frame and must not modify the guest output; the return value
// is ignored.
using NativeGuestOutputRenderer = bool (*)(
    const NativeGuestOutputRenderContext& context);

class NativeGuestOutputRendererRegistration {
 public:
  void Set(NativeGuestOutputRenderer renderer) {
    renderer_.store(renderer, std::memory_order_release);
  }
  NativeGuestOutputRenderer Get() const {
    return renderer_.load(std::memory_order_acquire);
  }
  bool IsRegistered() const { return Get() != nullptr; }
  bool Invoke(const NativeGuestOutputRenderContext& context) const {
    NativeGuestOutputRenderer renderer = Get();
    return renderer ? renderer(context) : false;
  }

 private:
  std::atomic<NativeGuestOutputRenderer> renderer_{nullptr};
};

enum class GraphicsShaderStage : uint32_t {
  kVertex = 1,
  kPixel = 2,
  // Host geometry shaders (guest hash 0, the key as the specialization).
  kGeometry = 3,
};

struct GraphicsShaderTextureBinding {
  uint32_t bindless_descriptor_index = 0;
  uint32_t fetch_constant = 0;
  uint32_t dimension = 0;
  uint32_t is_signed = 0;
};

struct GraphicsShaderSamplerBinding {
  uint32_t bindless_descriptor_index = 0;
  uint32_t fetch_constant = 0;
  uint32_t mag_filter = 0;
  uint32_t min_filter = 0;
  uint32_t mip_filter = 0;
  uint32_t aniso_filter = 0;
};

// The bytecode and binding views are borrowed for the duration of the callback
// only. The observer is diagnostic and cannot alter translation or draw state.
struct GraphicsShaderTranslationObservation {
  GraphicsShaderStage stage = GraphicsShaderStage::kVertex;
  uint64_t guest_hash = 0;
  uint64_t specialization_mask = 0;
  const uint8_t* bytecode = nullptr;
  size_t bytecode_size = 0;
  uint32_t translator_version = 0;
  // rex::graphics::Fh1ShaderPack::Backend and its device feature bits.
  uint32_t backend = 0;
  uint32_t device_features = 0;
  bool bindless_resources = false;
  bool edram_rov = false;
  bool gamma_render_target_as_unorm8 = false;
  bool msaa_2x = false;
  uint32_t draw_resolution_scale_x = 1;
  uint32_t draw_resolution_scale_y = 1;
  const GraphicsShaderTextureBinding* texture_bindings = nullptr;
  size_t texture_binding_count = 0;
  const GraphicsShaderSamplerBinding* sampler_bindings = nullptr;
  size_t sampler_binding_count = 0;
  uint32_t used_texture_mask = 0;
};

using GraphicsShaderTranslationObserver = void (*)(
    const GraphicsShaderTranslationObservation& observation);

struct GraphicsFinalDrawTextureIdentity {
  uint32_t fetch_constant = 0;
  uint32_t fetch_words[6] = {};
  uint64_t allocation_id = 0;
  uint64_t payload_generation = 0;
  uint32_t outdated_mask = 0;
};


class IGraphicsSystem {
 public:
  virtual ~IGraphicsSystem() = default;

  // Build the provider + presenter. Safe to call standalone (without a
  // Runtime) to stand up a window + ImGui for an installer. Idempotent.
  // Must be called before SetupGuestGpu if presentation is desired: some
  // backends (e.g. Vulkan) bake swapchain support into the provider, and
  // a headless provider from SetupGuestGpu cannot be upgraded in place.
  virtual X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) = 0;

  // Wire the GPU into the guest address space: MMIO, command processor,
  // vsync worker. Needs the Runtime's dispatcher + kernel state. If
  // SetupPresentation has not been called, a headless provider is built.
  virtual X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                 KernelState* kernel_state) = 0;

  virtual bool has_presentation() const = 0;

  // --- Optional capabilities, default no-op -------------------------------

  // Host presentation objects for ReXApp's overlay wiring; custom systems may
  // leave these null.
  virtual ui::GraphicsProvider* provider() const { return nullptr; }
  virtual ui::Presenter* presenter() const { return nullptr; }

  // Optional read-only shader translation observation; observers cannot alter
  // translation or draw behavior.
  virtual void SetShaderTranslationObserver(
      GraphicsShaderTranslationObserver observer) {
    (void)observer;
  }
  virtual void SetNativeGuestOutputRenderer(NativeGuestOutputRenderer renderer) {
    (void)renderer;
  }
  virtual bool HasNativeGuestOutputRenderer() const { return false; }

  // Guest GPU services reached from the xboxkrnl Vd* exports.
  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    (void)callback;
    (void)user_data;
  }
  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    (void)ptr;
    (void)size_log2;
  }
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
    (void)ptr;
    (void)block_size_log2;
  }

  // Persistent shader/pipeline storage under the cache root. Default: none.
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                       bool blocking) {
    (void)cache_root;
    (void)title_id;
    (void)blocking;
  }

  // One-shot convenience for callers that don't care about the split.
  X_STATUS Setup(runtime::FunctionDispatcher* function_dispatcher, KernelState* kernel_state,
                 ui::WindowedAppContext* app_context, bool with_presentation) {
    if (with_presentation && !has_presentation()) {
      X_STATUS status = SetupPresentation(app_context);
      if (XFAILED(status)) {
        return status;
      }
    }
    return SetupGuestGpu(function_dispatcher, kernel_state);
  }

  virtual void Shutdown() = 0;
};

}  // namespace rex::system
