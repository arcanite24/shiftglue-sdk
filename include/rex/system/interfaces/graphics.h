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

#include <atomic>
#include <cstdint>
#include <filesystem>

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
enum class NativeGuestOutputRetainedPassMode : uint32_t;
using NativeGuestOutputClearColor = bool (*)(
    const NativeGuestOutputRenderContext& context, const float color[4]);
using NativeGuestOutputDrawDiagnosticTriangle = bool (*)(
    const NativeGuestOutputRenderContext& context, uint32_t phase);
using NativeGuestOutputDrawRetainedPass = bool (*)(
    const NativeGuestOutputRenderContext& context,
    NativeGuestOutputRetainedPassMode mode);
class KernelState;
}

namespace rex::system {

enum class NativeGuestOutputBackend : uint32_t {
  kUnsupported = 0,
  kD3D12 = 1,
  kVulkan = 2,
};

enum class NativeGuestOutputRetainedPassMode : uint32_t {
  kPresentNative = 0,
  kCompareNative = 1,
  kCompareXenos = 2,
  kPrototypeNative = 3,
  kPrototypeHybrid = 4,
  kPrototypeCompareNative = 5,
  kPrototypeCompareXenos = 6,
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
  uint64_t retained_pass_frame_sequence = 0;
  bool use_pwl_gamma_ramp = false;
  bool xenos_fxaa_applied = false;
  NativeGuestOutputClearColor clear_color = nullptr;
  NativeGuestOutputDrawDiagnosticTriangle draw_diagnostic_triangle = nullptr;
  NativeGuestOutputDrawRetainedPass draw_retained_pass = nullptr;
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

enum class GraphicsNativeTextureBackend : uint32_t {
  kUnsupported = 0,
  kD3D12 = 1,
  kVulkan = 2,
};

using GraphicsNativeTextureRetain = void (*)(void* resource);
using GraphicsNativeTextureRelease = void (*)(void* resource);

// A GPU-ready texture resource borrowed for the duration of the observer. An
// observer that stores resource must call retain before returning and release
// it only after its own submission-safe retirement gate has passed.
struct GraphicsNativeTextureResourceObservation {
  uint32_t fetch_constant = 0;
  uint32_t fetch_dwords[6] = {};
  uint32_t base_address = 0;
  uint32_t base_length = 0;
  uint32_t mip_address = 0;
  uint32_t mip_length = 0;
  uint32_t guest_format = 0;
  uint32_t guest_dimension = 0;
  uint32_t guest_width = 0;
  uint32_t guest_height = 0;
  uint32_t guest_depth_or_array_size = 0;
  uint32_t guest_pitch = 0;
  uint32_t guest_row_pitch_bytes = 0;
  uint32_t guest_endianness = 0;
  uint32_t guest_mip_max_level = 0;
  uint32_t guest_tiled = 0;
  uint32_t guest_packed_mips = 0;
  uint32_t host_resource_format = 0;
  uint32_t host_view_format = 0;
  uint32_t host_swizzle = 0;
  uint32_t host_swizzled_signs = 0;
  uint32_t host_dimension = 0;
  uint32_t host_mip_levels = 0;
  uint32_t host_depth_or_array_size = 0;
  uint64_t host_width = 0;
  uint32_t host_height = 0;
  uint64_t host_allocation_bytes = 0;
  void* resource = nullptr;
  GraphicsNativeTextureRetain retain = nullptr;
  GraphicsNativeTextureRelease release = nullptr;
};

constexpr uint32_t kGraphicsNativeTextureResourceObservationLimit = 32;

struct GraphicsNativeTextureSetObservation {
  GraphicsNativeTextureBackend backend =
      GraphicsNativeTextureBackend::kUnsupported;
  uint32_t used_texture_mask = 0;
  uint32_t resource_count = 0;
  uint32_t resource_overflow = 0;
  uint64_t current_submission = 0;
  uint64_t completed_submission = 0;
  GraphicsNativeTextureResourceObservation
      resources[kGraphicsNativeTextureResourceObservationLimit] = {};
};

using GraphicsNativeTextureSetObserver = void (*)(
    const GraphicsNativeTextureSetObservation& observation);

// Dedicated resolve-provenance channel for native-renderer resource bridges.
// This is separate from the diagnostic copy observer so both may be installed
// without either observer replacing the other.
struct GraphicsCopyObservation;
using GraphicsNativeResolveObserver = void (*)(
    const GraphicsCopyObservation& observation);

struct GraphicsVertexBindingObservation {
  uint32_t fetch_constant = 0;
  uint32_t address = 0;
  uint32_t size = 0;
  uint32_t stride_words = 0;
  uint32_t endianness = 0;
};

constexpr uint32_t kGraphicsVertexBindingObservationLimit = 8;

struct GraphicsVertexAttributeObservation {
  uint32_t binding_index = 0;
  uint32_t fetch_constant = 0;
  int32_t offset_words = 0;
  uint32_t stride_words = 0;
  uint32_t data_format = 0;
  uint32_t fetch_word_mask = 0;
  int32_t exp_adjust = 0;
  uint32_t signed_rf_mode = 0;
  uint32_t result_storage_target = 0;
  uint32_t result_storage_index = 0;
  uint32_t result_write_mask = 0;
  uint32_t result_components = 0;
  uint32_t flags = 0;
};

constexpr uint32_t kGraphicsVertexAttributeObservationLimit = 32;

struct GraphicsFloatConstantObservation {
  uint32_t index = 0;
  uint32_t values[4] = {};
  uint32_t write_maximum_age_frames = 0;
  uint32_t write_provenance_valid = 0;
  uint32_t write_provenance_split = 0;
  uint32_t write_value_mismatch_mask = 0;
};

constexpr uint32_t kGraphicsFloatConstantObservationLimit = 64;

struct GraphicsTextureFetchObservation {
  uint32_t stage = 0;
  uint32_t fetch_constant = 0;
  uint32_t dwords[6] = {};
  uint32_t opcode = 0;
  uint32_t dimension = 0;
  uint32_t filters = 0;
  uint32_t flags = 0;
  uint32_t lod_bias = 0;
  uint32_t offsets = 0;
  uint32_t result_storage_target = 0;
  uint32_t result_storage_index = 0;
  uint32_t result_write_mask = 0;
  uint32_t result_components = 0;
};

constexpr uint32_t kGraphicsTextureFetchObservationLimit = 16;

struct GraphicsDrawObservation {
  uint64_t frame_sequence = 0;
  uint64_t draw_sequence = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t packet_physical_address = UINT32_MAX;
  uint32_t command_buffer_physical_address = UINT32_MAX;
  uint32_t command_buffer_length_dwords = 0;
  uint32_t command_buffer_parent_packet_physical_address = UINT32_MAX;
  uint32_t command_buffer_root_physical_address = UINT32_MAX;
  uint32_t command_buffer_depth = 0;
  uint64_t bin_select = 0;
  uint64_t bin_mask = 0;
  uint32_t packet = 0;
  uint32_t initiator = 0;
  uint32_t primitive_type = 0;
  uint32_t index_count = 0;
  uint32_t source_select = 0;
  uint32_t index_buffer_address = 0;
  uint32_t index_buffer_length = 0;
  uint32_t index_format = 0;
  uint32_t index_endianness = 0;
  uint32_t index_reset = 0;
  bool index_reset_enabled = false;
  uint32_t vertex_index_offset = 0;
  uint32_t vertex_index_min = 0;
  uint32_t vertex_index_max = 0;
  uint32_t vertex_binding_count = 0;
  GraphicsVertexBindingObservation
      vertex_bindings[kGraphicsVertexBindingObservationLimit] = {};
  uint32_t vertex_binding_overflow = 0;
  uint32_t vertex_attribute_count = 0;
  GraphicsVertexAttributeObservation
      vertex_attributes[kGraphicsVertexAttributeObservationLimit] = {};
  uint32_t vertex_attribute_overflow = 0;
  uint32_t surface_info = 0;
  uint32_t color_info[4] = {};
  uint32_t depth_info = 0;
  uint32_t window_scissor_tl = 0;
  uint32_t window_scissor_br = 0;
  uint32_t viewport_xscale = 0;
  uint32_t viewport_xoffset = 0;
  uint32_t viewport_yscale = 0;
  uint32_t viewport_yoffset = 0;
  uint32_t viewport_transform_control = 0;
  uint32_t rb_modecontrol = 0;
  uint32_t viz_query_condition = 0;
  uint32_t pa_sc_viz_query = 0;
  uint32_t texture_fetch_mask = 0;
  uint32_t rb_color_mask = 0;
  uint32_t rb_blendcontrol[4] = {};
  uint32_t rb_depthcontrol = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t pa_su_vtx_cntl = 0;
  uint32_t texture_fetch_addresses[32] = {};
  uint32_t texture_fetch_mip_addresses[32] = {};
  uint32_t texture_fetch_base_lengths[32] = {};
  uint32_t texture_fetch_mip_lengths[32] = {};
  uint32_t texture_fetch_layout_valid_mask = 0;
  uint32_t vertex_float_constant_count = 0;
  GraphicsFloatConstantObservation vertex_float_constants
      [kGraphicsFloatConstantObservationLimit] = {};
  uint32_t vertex_float_constant_overflow = 0;
  uint32_t pixel_float_constant_count = 0;
  GraphicsFloatConstantObservation pixel_float_constants
      [kGraphicsFloatConstantObservationLimit] = {};
  uint32_t pixel_float_constant_overflow = 0;
  uint32_t bool_constant_bitmap[8] = {};
  uint32_t bool_constant_values[8] = {};
  uint32_t loop_constant_bitmap = 0;
  uint32_t loop_constant_values[32] = {};
  uint32_t texture_state_count = 0;
  GraphicsTextureFetchObservation
      texture_states[kGraphicsTextureFetchObservationLimit] = {};
  uint32_t texture_state_overflow = 0;
  bool indexed = false;
  bool major_mode_explicit = false;
  bool vertex_memexport = false;
};

using GraphicsDrawObserver = void (*)(const GraphicsDrawObservation& observation);

struct GraphicsShaderConstantWriteObservation {
  uint64_t frame_sequence = 0;
  uint32_t packet_physical_address = UINT32_MAX;
  uint32_t command_buffer_physical_address = UINT32_MAX;
  uint32_t command_buffer_length_dwords = 0;
  uint32_t command_buffer_parent_packet_physical_address = UINT32_MAX;
  uint32_t command_buffer_root_physical_address = UINT32_MAX;
  uint32_t command_buffer_depth = 0;
  uint32_t packet = 0;
  uint32_t register_index = 0;
  uint32_t value = 0;
};

using GraphicsShaderConstantWriteObserver = void (*)(
    const GraphicsShaderConstantWriteObservation& observation);

struct GraphicsIndirectBufferObservation {
  uint32_t packet_physical_address = UINT32_MAX;
  uint32_t parent_buffer_physical_address = UINT32_MAX;
  uint32_t parent_buffer_length_dwords = 0;
  uint32_t target_buffer_physical_address = UINT32_MAX;
  uint32_t target_buffer_length_dwords = 0;
  uint32_t root_buffer_physical_address = UINT32_MAX;
  uint32_t depth = 0;
  uint32_t opcode = 0;
  bool entering = false;
};

using GraphicsIndirectBufferObserver = void (*)(
    const GraphicsIndirectBufferObservation& observation);

struct GraphicsCopyObservation {
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
  bool succeeded = false;
};

using GraphicsCopyObserver = void (*)(const GraphicsCopyObservation& observation);

enum class GraphicsNativeFrameAccumulatorStatus : uint32_t {
  kRecorded = 1,
  kCancelled = 2,
  kInvalidRequest = 3,
  kUnavailable = 4,
  kUnsupportedTarget = 5,
  kAllocationFailed = 6,
  kUnqualifiedSource = 7,
};

struct GraphicsNativeFrameAccumulatorResult {
  GraphicsNativeFrameAccumulatorStatus status =
      GraphicsNativeFrameAccumulatorStatus::kInvalidRequest;
  uint64_t frame_sequence = 0;
  uint32_t resource_width = 0;
  uint32_t resource_height = 0;
  uint32_t logical_width = 0;
  uint32_t logical_height = 0;
  uint32_t appended_row_end = 0;
  uint32_t source_resource_width = 0;
  uint32_t source_resource_height = 0;
  uint32_t source_sample_count = 0;
  uint32_t source_sample_quality = 0;
  uint32_t source_guest_msaa_samples = 0;
  uint32_t draw_resolution_scale_x = 0;
  uint32_t draw_resolution_scale_y = 0;
  bool native_2x_msaa = false;
  bool committed = false;
};

using GraphicsNativeFrameAccumulatorCompletion = void (*)(
    const GraphicsNativeFrameAccumulatorResult& result);

// A fail-closed request to copy rows from the current private isolated color
// replay target into a private single-sample frame accumulator. The request is
// produced only after the authoritative Xenos resolve succeeds. It cannot
// alter the guest resolve, publish to guest memory, or suppress a guest draw.
struct GraphicsNativeFrameAccumulatorRequest {
  uint32_t logical_width = 0;
  uint32_t logical_height = 0;
  uint32_t storage_height = 0;
  uint32_t destination_row = 0;
  uint32_t storage_row_count = 0;
  bool begin = false;
  bool append = false;
  bool commit = false;
  bool cancel = false;
  GraphicsNativeFrameAccumulatorCompletion completion = nullptr;
};

using GraphicsNativeFrameAccumulatorPlanner = bool (*)(
    const GraphicsCopyObservation& observation,
    GraphicsNativeFrameAccumulatorRequest& request_out);

enum class GraphicsShaderStage : uint32_t {
  kVertex = 1,
  kPixel = 2,
};

// The bytecode view is borrowed for the duration of the callback only. The
// observer is diagnostic and cannot alter shader translation or draw state.
struct GraphicsShaderTranslationObservation {
  GraphicsShaderStage stage = GraphicsShaderStage::kVertex;
  uint64_t guest_hash = 0;
  uint64_t specialization_mask = 0;
  const uint8_t* bytecode = nullptr;
  size_t bytecode_size = 0;
};

using GraphicsShaderTranslationObserver = void (*)(
    const GraphicsShaderTranslationObservation& observation);

struct GraphicsPreparedDrawObservation {
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
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t bound_render_target_bits = 0;
  uint32_t bound_render_target_formats[5] = {};
  uint32_t flags = 0;
};

using GraphicsPreparedDrawObserver = void (*)(
    const GraphicsPreparedDrawObservation& observation);

enum class GraphicsDrawOutcomeStatus : uint32_t {
  kCompleted = 1,
  kEdramCopy = 2,
  kMissingVertexShader = 3,
  kZeroSurfacePitch = 4,
  kNoRasterizationOrMemexport = 5,
  kSubmissionFailed = 6,
  kPrimitiveProcessingFailed = 7,
  kNoHostVertices = 8,
  kRenderTargetUpdateFailed = 9,
  kPipelineConfigurationFailed = 10,
  kPipelinePending = 11,
  kBindingUpdateFailed = 12,
  kInvalidVertexFetch = 13,
  kVertexResidencyFailed = 14,
  kMemexportResidencyFailed = 15,
  kUnsupportedPrimitive = 16,
  kScratchIndexBufferFailed = 17,
  kUnsupportedIndexBuffer = 18,
};

// Emitted exactly once for each D3D12 draw submission attempt. This is a
// passive diagnostic result and cannot alter whether the backend executes the
// draw, copy, or no-effect path.
struct GraphicsDrawOutcomeObservation {
  GraphicsDrawOutcomeStatus status = GraphicsDrawOutcomeStatus::kCompleted;
  uint64_t frame_sequence = 0;
  uint64_t draw_sequence = 0;
  uint32_t packet_physical_address = UINT32_MAX;
  bool prepared = false;
  bool succeeded = false;
};

using GraphicsDrawOutcomeObserver = void (*)(
    const GraphicsDrawOutcomeObservation& observation);

enum class GraphicsIsolatedDrawStatus : uint32_t {
  kRecorded = 1,
  kUnsupportedState = 2,
  kTargetCreationFailed = 3,
};

enum class GraphicsIsolatedDrawTargetFailure : uint32_t {
  kNone = 0,
  kIncompatibleModes = 1,
  kUnsupportedPath = 2,
  kMissingDepthTarget = 3,
  kUnexpectedDepthTarget = 4,
  kMissingColorTarget = 5,
  kUnexpectedAdditionalColorTarget = 6,
  kDepthTargetCreationFailed = 7,
  kColorTargetCreationFailed = 8,
  kInvalidLogicalExtent = 9,
  kDepthFormatUnavailable = 10,
  kRetainedTargetUnavailable = 11,
  kRetainedTargetMismatch = 12,
};

struct GraphicsIsolatedDrawResult {
  GraphicsIsolatedDrawStatus status =
      GraphicsIsolatedDrawStatus::kUnsupportedState;
  uint64_t frame_sequence = 0;
  bool frame_accumulator_source = false;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  GraphicsIsolatedDrawTargetFailure target_failure =
      GraphicsIsolatedDrawTargetFailure::kNone;
};

using GraphicsIsolatedDrawCompletion = void (*)(
    const GraphicsIsolatedDrawResult& result);

enum class GraphicsIsolatedDrawReadbackStatus : uint32_t {
  kReady = 1,
  kUnsupportedTarget = 2,
  kResolveAllocationFailed = 3,
  kAllocationFailed = 4,
  kMapFailed = 5,
};

struct GraphicsIsolatedDrawReadback {
  static constexpr uint32_t kMaxPlanes = 2;
  GraphicsIsolatedDrawReadbackStatus status =
      GraphicsIsolatedDrawReadbackStatus::kUnsupportedTarget;
  uint32_t detail = 0;
  const uint8_t* data = nullptr;
  uint64_t data_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_pitch = 0;
  uint32_t format = 0;
  uint32_t sample_count = 1;
  uint32_t plane_count = 0;
  uint64_t plane_offsets[kMaxPlanes] = {};
  uint32_t plane_row_pitches[kMaxPlanes] = {};
  uint32_t plane_row_sizes[kMaxPlanes] = {};
  uint32_t plane_row_counts[kMaxPlanes] = {};
};

using GraphicsIsolatedDrawReadbackCompletion = void (*)(
    const GraphicsIsolatedDrawReadback& readback);

enum class GraphicsIsolatedDrawPublicationStatus : uint32_t {
  kPublished = 1,
  kUnsupportedPath = 2,
  kUnavailable = 3,
  kTargetMismatch = 4,
};

struct GraphicsIsolatedDrawPublicationResult {
  GraphicsIsolatedDrawPublicationStatus status =
      GraphicsIsolatedDrawPublicationStatus::kUnavailable;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  uint32_t sample_count = 0;
  bool color_published = false;
  bool depth_stencil_published = false;
  // True only when publication completed before the authoritative guest draw
  // and the backend consequently omitted that draw. A failed or incomplete
  // publication must leave this false and execute the original guest draw.
  bool guest_draw_suppressed = false;
};

using GraphicsIsolatedDrawPublicationCompletion = void (*)(
    const GraphicsIsolatedDrawPublicationResult& result);

struct GraphicsIsolatedDrawRequest {
  bool requested = false;
  // Duplicate a draw that has only a depth/stencil attachment. The backend
  // must reject this mode if any guest color target is bound.
  bool depth_only_target = false;
  // Duplicate a draw that has only the first color attachment. The backend
  // must reject this mode if a guest depth/stencil or later color target is
  // bound. Mutually exclusive with depth_only_target.
  bool color_only_target = false;
  bool readback_requested = false;
  // Capture the authoritative guest color target after the original draw.
  // This is independent of the private replay readback and never suppresses
  // or redirects the guest draw.
  bool reference_readback_requested = false;
  // Capture the private and authoritative depth/stencil attachments after the
  // same pass follower. These readbacks are diagnostic only and never suppress
  // or redirect the guest draw.
  bool depth_readback_requested = false;
  bool reference_depth_readback_requested = false;
  // Capture the authoritative copy source and private copy destination
  // consecutively after the private target is seeded and before either
  // duplicate draw is recorded. These checkpoints distinguish copy or
  // initialization faults from draw-state divergence.
  bool seed_depth_readback_requested = false;
  bool reference_seed_depth_readback_requested = false;
  // Before seeding the private target, clear only its stencil plane to a
  // sentinel. The subsequent guest-to-private copy must overwrite it. This is
  // a one-shot diagnostic and never touches the authoritative guest target.
  bool stencil_seed_probe_requested = false;
  bool reference_marker_requested = false;
  // Mark the authoritative guest draw as an exact later-consumer-family
  // target. This never duplicates, redirects, or suppresses the draw.
  bool consumer_reference_marker_requested = false;
  // Capture the authoritative guest color target immediately before and after
  // one exact consumer-family draw. Both copies are diagnostic-only and the
  // original draw remains authoritative.
  bool consumer_reference_readback_requested = false;
  // Capture the authoritative guest depth/stencil target around the same
  // exact consumer draw. This remains independent from color readback and
  // never changes draw execution or attachment ownership.
  bool consumer_reference_depth_readback_requested = false;
  bool retain_target = false;
  // Rebind the retained private target without copying the guest target. This
  // is valid only for the immediately following draw of an isolated pass.
  bool reuse_target = false;
  // Accumulate this draw into the private target, but do not make that target
  // visible to the guest-output callback until the matching swap commits the
  // complete frame. Any failed draw in the accumulation cancels the pending
  // preview and leaves the previous Xenos frame authoritative.
  bool defer_preview_publication_until_swap = false;
  // Make the completed private color replay eligible as the source of one
  // same-frame native accumulator append. The accumulator consumes this tag;
  // unrelated isolated replays can never become procedural input implicitly.
  bool frame_accumulator_source = false;
  // After the original authoritative guest draw, publish the completed private
  // pass into the same guest color and depth/stencil resources. The backend
  // must validate the entire pair before recording either copy. This never
  // skips a draw, resolve, query, event, fence, or memexport operation.
  bool publish_to_guest_requested = false;
  // Publish the completed isolated replay before the guest draw and omit only
  // that draw if the full color and depth/stencil pair was published. This is
  // fail-closed: replay or publication failure executes the guest draw.
  bool suppress_guest_draw_if_published = false;
  uint64_t frame_sequence = 0;
  GraphicsIsolatedDrawCompletion completion = nullptr;
  GraphicsIsolatedDrawReadbackCompletion readback_completion = nullptr;
  GraphicsIsolatedDrawReadbackCompletion reference_readback_completion =
      nullptr;
  GraphicsIsolatedDrawReadbackCompletion depth_readback_completion = nullptr;
  GraphicsIsolatedDrawReadbackCompletion
      reference_depth_readback_completion = nullptr;
  GraphicsIsolatedDrawReadbackCompletion seed_depth_readback_completion =
      nullptr;
  GraphicsIsolatedDrawReadbackCompletion
      reference_seed_depth_readback_completion = nullptr;
  GraphicsIsolatedDrawReadbackCompletion
      consumer_reference_before_readback_completion = nullptr;
  GraphicsIsolatedDrawReadbackCompletion
      consumer_reference_after_readback_completion = nullptr;
  GraphicsIsolatedDrawReadbackCompletion
      consumer_reference_before_depth_readback_completion = nullptr;
  GraphicsIsolatedDrawReadbackCompletion
      consumer_reference_after_depth_readback_completion = nullptr;
  GraphicsIsolatedDrawPublicationCompletion publication_completion = nullptr;
};

// Called after the host pipeline has been prepared. A request duplicates the
// current draw into a private backend target and never skips the guest draw.
using GraphicsIsolatedDrawRequestObserver = void (*)(
    const GraphicsPreparedDrawObservation& observation,
    GraphicsIsolatedDrawRequest& request);

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
  virtual void SetDrawObserver(GraphicsDrawObserver observer) {
    (void)observer;
  }
  virtual void SetShaderConstantWriteObserver(
      GraphicsShaderConstantWriteObserver observer) {
    (void)observer;
  }
  virtual void SetIndirectBufferObserver(
      GraphicsIndirectBufferObserver observer) {
    (void)observer;
  }
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
  virtual void SetDrawOutcomeObserver(GraphicsDrawOutcomeObserver observer) {
    (void)observer;
  }
  virtual void SetIsolatedDrawRequestObserver(
      GraphicsIsolatedDrawRequestObserver observer) {
    (void)observer;
  }
  virtual void SetNativeTextureSetObserver(
      GraphicsNativeTextureSetObserver observer) {
    (void)observer;
  }
  virtual void SetNativeResolveObserver(
      GraphicsNativeResolveObserver observer) {
    (void)observer;
  }
  virtual void SetNativeFrameAccumulatorPlanner(
      GraphicsNativeFrameAccumulatorPlanner planner) {
    (void)planner;
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
