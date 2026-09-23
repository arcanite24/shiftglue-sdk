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

struct NativeGuestOutputRenderContext {
  NativeGuestOutputBackend backend = NativeGuestOutputBackend::kUnsupported;
  uint32_t guest_output_width = 0;
  uint32_t guest_output_height = 0;
  uint32_t display_width = 0;
  uint32_t display_height = 0;
  uint32_t output_format = 0;
  void* device = nullptr;
  void* command_context = nullptr;
  void* guest_output = nullptr;
  uint64_t submission = 0;
  uint64_t frame_sequence = 0;
  bool use_pwl_gamma_ramp = false;
  bool xenos_fxaa_applied = false;
};

// Returning false yields without modifying guest output. A callback that has
// recorded any output command must return true.
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

// Stable, title-local execution identity for Pinyon Shift's FH1 corpus. This
// intentionally isn't a generic Xenos key: changing the fields or their
// normalization requires a local corpus rebuild.
enum class GraphicsFh1ExecutionKind : uint32_t {
  kDraw = 1,
  kCopyResolve = 2,
};

struct GraphicsFh1ExecutionKey {
  static constexpr uint32_t kVersion = 2;
  static constexpr size_t kSerializedSize = 72;

  uint32_t version = kVersion;
  GraphicsFh1ExecutionKind kind = GraphicsFh1ExecutionKind::kDraw;
  uint32_t hazard_flags = 0;
  uint32_t flags = 0;
  uint64_t identity = 0;
  uint64_t shader_state = 0;
  uint64_t pipeline_state = 0;
  uint64_t attachment_state = 0;
  uint64_t resource_state = 0;
  uint64_t dynamic_state = 0;
  uint64_t operation_state = 0;

  bool operator==(const GraphicsFh1ExecutionKey&) const = default;

  uint64_t ComputeIdentity() const {
    uint64_t hash = 0xCBF29CE484222325ull;
    const auto append = [&hash](uint64_t value) {
      for (uint32_t byte = 0; byte < 8; ++byte) {
        hash ^= uint8_t(value >> (byte * 8));
        hash *= 0x100000001B3ull;
      }
    };
    append(version);
    append(uint32_t(kind));
    append(hazard_flags);
    append(flags);
    append(shader_state);
    append(pipeline_state);
    append(attachment_state);
    append(resource_state);
    append(dynamic_state);
    append(operation_state);
    return hash ? hash : 1;
  }

  std::array<uint8_t, kSerializedSize> Serialize() const {
    std::array<uint8_t, kSerializedSize> bytes{};
    size_t offset = 0;
    const auto write = [&bytes, &offset](uint64_t value, size_t size) {
      for (size_t byte = 0; byte < size; ++byte) {
        bytes[offset++] = uint8_t(value >> (byte * 8));
      }
    };
    write(version, 4);
    write(uint32_t(kind), 4);
    write(hazard_flags, 4);
    write(flags, 4);
    write(identity, 8);
    write(shader_state, 8);
    write(pipeline_state, 8);
    write(attachment_state, 8);
    write(resource_state, 8);
    write(dynamic_state, 8);
    write(operation_state, 8);
    return bytes;
  }

  static bool Deserialize(std::span<const uint8_t> bytes,
                          GraphicsFh1ExecutionKey& key_out) {
    if (bytes.size() != kSerializedSize) {
      return false;
    }
    size_t offset = 0;
    const auto read = [&bytes, &offset](size_t size) {
      uint64_t value = 0;
      for (size_t byte = 0; byte < size; ++byte) {
        value |= uint64_t(bytes[offset++]) << (byte * 8);
      }
      return value;
    };
    GraphicsFh1ExecutionKey key;
    key.version = uint32_t(read(4));
    key.kind = GraphicsFh1ExecutionKind(uint32_t(read(4)));
    key.hazard_flags = uint32_t(read(4));
    key.flags = uint32_t(read(4));
    key.identity = read(8);
    key.shader_state = read(8);
    key.pipeline_state = read(8);
    key.attachment_state = read(8);
    key.resource_state = read(8);
    key.dynamic_state = read(8);
    key.operation_state = read(8);
    if (key.version != kVersion ||
        (key.kind != GraphicsFh1ExecutionKind::kDraw &&
         key.kind != GraphicsFh1ExecutionKind::kCopyResolve) ||
        key.identity != key.ComputeIdentity()) {
      return false;
    }
    key_out = key;
    return true;
  }
};

struct GraphicsCopyObservation {
  GraphicsFh1ExecutionKey fh1_execution_key;
  uint64_t frame_sequence = 0;
  uint64_t copy_sequence = 0;
  uint64_t current_submission = 0;
  uint64_t completed_submission = 0;
  uint32_t written_address = 0;
  uint32_t written_length = 0;
  uint32_t rb_copy_control = 0;
  uint32_t rb_copy_dest_base = 0;
  uint32_t rb_copy_dest_info = 0;
  uint32_t rb_copy_dest_pitch = 0;
  uint32_t surface_info = 0;
  uint32_t color_info[4] = {};
  uint32_t depth_info = 0;
  uint32_t source_resource_width = 0;
  uint32_t source_resource_height = 0;
  uint32_t source_resource_format = 0;
  uint32_t source_sample_count = 0;
  uint32_t source_sample_quality = 0;
  uint32_t source_guest_msaa_samples = 0;
  uint32_t draw_resolution_scale_x = 0;
  uint32_t draw_resolution_scale_y = 0;
  uint32_t source_target_base_tiles = 0;
  uint32_t source_target_pitch_tiles_at_32bpp = 0;
  uint32_t resolve_source_base_tiles = 0;
  uint32_t resolve_source_pitch_tiles = 0;
  uint32_t resolve_source_format = 0;
  uint32_t resolve_source_guest_msaa_samples = 0;
  uint32_t resolve_guest_offset_x = 0;
  uint32_t resolve_guest_offset_y = 0;
  uint32_t resolve_guest_width = 0;
  uint32_t resolve_guest_height = 0;
  uint32_t resolve_physical_offset_x = 0;
  uint32_t resolve_physical_offset_y = 0;
  uint32_t resolve_physical_width = 0;
  uint32_t resolve_physical_height = 0;
  uint32_t resolve_dest_offset_x = 0;
  uint32_t resolve_dest_offset_y = 0;
  uint32_t resolve_dest_pitch = 0;
  uint32_t resolve_dest_height = 0;
  uint32_t resolve_sample_select = 0;
  bool resolve_info_valid = false;
  bool source_target_available = false;
  bool native_2x_msaa = false;
  bool succeeded = false;
};

using GraphicsCopyObserver = void (*)(const GraphicsCopyObservation& observation);

enum class GraphicsShaderStage : uint32_t {
  kVertex = 1,
  kPixel = 2,
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
  uint32_t vendor_id = 0;
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

enum class GraphicsFh1ExecutionMode : uint32_t {
  kCompatibility = 0,
  kCoveredInPlace = 1,
};

enum class GraphicsFh1FallbackReason : uint32_t {
  kNone = 0,
  kManifestUnavailable = 1,
  kExecutionKeyNotCaptured = 2,
  kHazardousDraw = 3,
  kPipelineNotPrewarmed = 4,
};

struct GraphicsPreparedDrawVertexFetch {
  uint32_t fetch_constant = 0;
  uint32_t stride_words = 0;
  uint32_t guest_base = 0;
  uint32_t length = 0;
  uint32_t type = 0;
  uint32_t source_packet_physical_0 = 0;
  uint32_t source_packet_physical_1 = 0;
  uint64_t source_execution_0 = 0;
  uint64_t source_execution_1 = 0;
  // SNR03 probe: 0=not attempted, 1=CPU snapshot, 2=non-CPU/rejected,
  // 3=per-range limit, 4=per-frame limit.
  uint32_t cpu_snapshot_status = 0;
  uint64_t cpu_snapshot_hash = 0;
  // Borrowed until the prepared-draw observer returns; valid only on success.
  const uint8_t* cpu_snapshot_bytes = nullptr;
};

struct GraphicsPreparedDrawTextureFetch {
  uint32_t fetch_constant = 0;
  uint32_t type = 0;
  uint32_t base_address = 0;
  uint32_t mip_address = 0;
  uint32_t format = 0;
  uint32_t dimension = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stack_depth = 0;
};

struct GraphicsPreparedDrawObservation {
  GraphicsFh1ExecutionKey fh1_execution_key;
  GraphicsFh1ExecutionMode fh1_execution_mode =
      GraphicsFh1ExecutionMode::kCompatibility;
  GraphicsFh1FallbackReason fh1_fallback_reason =
      GraphicsFh1FallbackReason::kManifestUnavailable;
  uint64_t fh1_runtime_shader_translations = 0;
  uint64_t fh1_runtime_sync_pipeline_creations = 0;
  uint64_t fh1_prepare_cpu_time_ns = 0;
  uint64_t frame_sequence = 0;
  uint64_t draw_sequence = 0;
  uint64_t indirect_buffer_execution_id = 0;
  uint64_t indirect_buffer_parent_execution_id = 0;
  uint32_t indirect_dispatch_packet_physical_address = 0;
  uint32_t draw_packet_physical_address = 0;
  uint32_t command_buffer_physical_address = 0;
  uint32_t command_buffer_bytes = 0;
  uint32_t command_buffer_end_offset = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint64_t vertex_specialization_mask = 0;
  uint64_t pixel_specialization_mask = 0;
  uint32_t guest_primitive_type = 0;
  uint32_t host_primitive_type = 0;
  uint32_t host_vertex_shader_type = 0;
  uint32_t tessellation_mode = 0;
  uint32_t index_buffer_type = 0;
  uint32_t host_index_format = 0;
  uint32_t host_primitive_reset_enabled = 0;
  uint32_t index_count = 0;
  uint32_t index_buffer_guest_base = 0;
  uint32_t index_buffer_length = 0;
  // Borrowed for the duration of the callback; count may exceed the
  // diagnostic array capacity, in which case only the first entries exist.
  const GraphicsPreparedDrawVertexFetch* vertex_fetches = nullptr;
  uint32_t vertex_fetch_count = 0;
  uint32_t vertex_fetch_capacity = 0;
  // First of 512 float4 registers, borrowed until the observer returns.
  const uint32_t* vertex_float_constant_words = nullptr;
  // Pixel shader's 256-register bitmap; indexes address the second half of
  // vertex_float_constant_words. Borrowed until the observer returns.
  const uint64_t* pixel_float_constant_bitmap = nullptr;
  uint32_t pixel_float_constant_count = 0;
  const GraphicsPreparedDrawTextureFetch* texture_fetches = nullptr;
  uint32_t texture_fetch_count = 0;
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t bound_render_target_bits = 0;
  uint32_t bound_render_target_formats[5] = {};
  uint32_t surface_info = 0;
  uint32_t color_info[4] = {};
  uint32_t depth_info = 0;
  uint32_t flags = 0;
};

using GraphicsPreparedDrawObserver = void (*)(
    const GraphicsPreparedDrawObservation& observation);

// Borrowed only during the callback, after the draw's system constants have
// reached their final values and before their GPU upload.
struct GraphicsFinalDrawStateObservation {
  uint64_t frame_sequence = 0;
  uint64_t draw_sequence = 0;
  uint32_t draw_packet_physical_address = 0;
  uint64_t dynamic_state = 0;
  const uint32_t* system_constant_words = nullptr;
  uint32_t system_constant_word_count = 0;
  const uint32_t* fetch_47_words = nullptr;
};
using GraphicsFinalDrawStateObserver = void (*)(
    const GraphicsFinalDrawStateObservation& observation);

struct GraphicsIndirectBufferObservation {
  uint64_t frame_sequence = 0;
  uint64_t execution_id = 0;
  uint64_t parent_execution_id = 0;
  uint32_t dispatch_packet_physical_address = 0;
  uint32_t command_buffer_physical_address = 0;
  uint32_t command_buffer_bytes = 0;
};

using GraphicsIndirectBufferObserver = void (*)(
    const GraphicsIndirectBufferObservation& observation);

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

  // Optional read-only command-stream observation. Backends invoke this
  // before draw submission and observers cannot alter draw behavior.
  virtual void SetCopyObserver(GraphicsCopyObserver observer) {
    (void)observer;
  }
  virtual void SetShaderTranslationObserver(
      GraphicsShaderTranslationObserver observer) {
    (void)observer;
  }
  virtual void SetPreparedDrawObserver(GraphicsPreparedDrawObserver observer) {
    (void)observer;
  }
  virtual void SetFinalDrawStateObserver(GraphicsFinalDrawStateObserver observer) {
    (void)observer;
  }
  virtual void SetIndirectBufferObserver(
      GraphicsIndirectBufferObserver observer) {
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
