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

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <sstream>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/fh1_geometry.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/fh1_mip_contract.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(fh1_post_chain_probe, false, "GPU/D3D12",
                    "Log resolve publications and their texture consumers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_mip_decode_probe, false, "GPU/D3D12",
                    "Measure reflection mip contract and packet replay CPU time")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(fh1_native_reflection_mips, true, "GPU/D3D12",
                    "Use experimental native reflection mips at symmetric 1x/2x; "
                    "fall back when the current command/input contract does not match")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_owned_depth_clear, true, "GPU/D3D12",
                    "Native owned depth clear at 1x; scaled rendering keeps compatibility transfers");
REXCVAR_DEFINE_BOOL(fh1_owned_depth_tile_clear, false, "GPU/D3D12",
                    "Experimental full-tile base-0 depth/stencil clear ownership");
REXCVAR_DEFINE_BOOL(fh1_recycle_geometry_buffers, false, "GPU/D3D12",
                    "Reuse the oldest completed same-sized geometry buffer under cache pressure");
REXCVAR_DEFINE_BOOL(fh1_contain_geometry_windows, false, "GPU/D3D12",
                    "Reuse the smallest same-base geometry window containing the request")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_cache_geometry_rejections, false, "GPU/D3D12",
                    "Skip repeated owned-geometry admission work until cache state changes");
REXCVAR_DEFINE_INT32(fh1_geometry_cache_mb, 32, "GPU/D3D12",
                     "Owned-geometry cache budget in MiB (8-128)");

REXCVAR_DEFINE_BOOL(d3d12_bindless, true, "GPU/D3D12", "Use bindless resources where available")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, false, "GPU/D3D12",
                    "Submit command list when PM4 primary buffer ends")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(fh1_discovery_sampling, false, "GPU/D3D12",
                    "Observe one complete source frame in 60 for manual discovery")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {

static bool Fh1ObserveCorpusFrame(uint64_t frame) {
  return !REXCVAR_GET(fh1_discovery_sampling) || frame % 60 == 0;
}

static bool Fh1GpuCorpusEnabled() {
  static const bool enabled =
      rex::cvar::GetFlagByName("pinyon_shift_fh1_gpu_corpus") == "true";
  return enabled;
}

static bool Fh1SceneDumpEnabled() {
  static const bool enabled =
      rex::cvar::GetFlagByName("pinyon_shift_fh1_scene_dump") == "true";
  return enabled;
}

static uint64_t HashFh1ExecutionValue(uint64_t hash, uint64_t value) {
  for (uint32_t byte = 0; byte < 8; ++byte) {
    hash ^= uint8_t(value >> (byte * 8));
    hash *= 0x100000001B3ull;
  }
  return hash;
}

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_extreme_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_tonemap_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_tonemap_vs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_velocity_dilate_ps.h"
}  // namespace shaders

D3D12CommandProcessor::D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), deferred_command_list_(*this) {}
D3D12CommandProcessor::~D3D12CommandProcessor() = default;

void D3D12CommandProcessor::UpdateDebugMarkersEnabled() {
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void D3D12CommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.BeginDebugMarker(label);
}

void D3D12CommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_list_.EndDebugMarker();
}

void D3D12CommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.InsertDebugMarker(label);
}

void D3D12CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void D3D12CommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void D3D12CommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void D3D12CommandProcessor::InvalidateVertexBufferResidency(uint32_t vfetch_index) {
  if (vfetch_index >= vertex_buffer_states_.size()) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
}

void D3D12CommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                 uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void D3D12CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

bool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  ZPDMode mode = GetZPDMode();
  if (mode == ZPDMode::kFast || mode == ZPDMode::kStrict) {
    return ExecuteModernZPD(reader, packet, count);
  }
  if (mode == ZPDMode::kFake) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);
  if (!sample_counts) {
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      sample_counts->ZPass_A = fake_sample_count;
      sample_counts->Total_A = fake_sample_count;
    }
    return true;
  };

  bool is_end_via_z_pass =
      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
  bool is_end_via_z_fail =
      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
  bool is_end = is_end_via_z_pass || is_end_via_z_fail;

  if (!is_end) {
    if (active_occlusion_query_.valid &&
        active_occlusion_query_.sample_count_address != sample_count_addr) {
      DisableHostOcclusionQueries();
      return write_fallback_result();
    }
    if (!BeginGuestOcclusionQuery(sample_count_addr)) {
      return write_fallback_result();
    }
    return true;
  }

  if (!active_occlusion_query_.valid ||
      active_occlusion_query_.sample_count_address != sample_count_addr) {
    DisableHostOcclusionQueries();
    return write_fallback_result();
  }

  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {
    return write_fallback_result();
  }

  return true;
}

ZPDMode D3D12CommandProcessor::GetZPDMode() const {
  const std::string& mode = REXCVAR_GET(occlusion_query);
  if (mode == "fake") {
    return ZPDMode::kFake;
  }
  if (mode == "fast") {
    return ZPDMode::kFast;
  }
  if (mode == "strict") {
    return ZPDMode::kStrict;
  }
  return ZPDMode::kLegacy;
}

bool D3D12CommandProcessor::ExecuteModernZPD(memory::RingBuffer* reader, uint32_t packet,
                                             uint32_t count) {
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    PERF_counter_inc(kZpdFakeFallbacks);
    // The packet payload has already been consumed, so reproduce the safe fake
    // completion locally instead of delegating to the base packet reader.
    uint32_t address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
    auto* report = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
    if (report && XenosZPDReport::HasPendingSentinel(report)) {
      XenosZPDReport::WriteSampleCount(report,
                                       uint32_t(REXCVAR_GET(query_occlusion_fake_sample_count)));
    }
    return true;
  }

  RetireModernZPDQueries();
  if (!BeginSubmission(true)) {
    PERF_counter_inc(kZpdFakeFallbacks);
    return true;
  }

  uint32_t address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  uint32_t record_base = XenosZPDReport::GetRecordBase(address);
  auto* guest_report =
      record_base ? memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(record_base)
                  : nullptr;
  if (!guest_report) {
    PERF_counter_inc(kZpdMalformedRecords);
    return true;
  }

  bool logical_current = zpd_lifecycle_.active().logical_active &&
                         zpd_lifecycle_.active().slot_base == XenosZPDReport::GetSlotBase(address);
  ZPDClassificationResult classification = ClassifyZPD(address, guest_report, logical_current);
  LogZPDObservation(address, guest_report, logical_current, classification);

  if (classification.classification == ZPDClassification::kBegin) {
    // A new BEGIN implicitly closes an older logical lifetime.
    if (zpd_lifecycle_.active().logical_active) {
      ZPDLifecycle::Report* old_report = zpd_lifecycle_.active_report();
      ZPDLifecycle::ReportHandle old_handle = zpd_lifecycle_.active().report_handle;
      uint32_t old_end = old_report ? old_report->end_record : 0;
      if (old_end) {
        CloseModernZPDSegment();
        zpd_lifecycle_.End(old_end);
        PERF_counter_inc(kZpdReportsEnded);
        old_report = zpd_lifecycle_.Find(old_handle);
        if (old_report && old_report->pending_segments == 0) {
          WriteModernZPDReport(*old_report, 1);
          ZPDLifecycle::Report abandoned;
          bool commit = false;
          zpd_lifecycle_.Abandon(old_handle, 1, abandoned, commit);
          PERF_counter_inc(kZpdFakeFallbacks);
        } else if (old_report && GetZPDMode() == ZPDMode::kFast) {
          uint32_t speculative = old_report->has_cached_delta ? old_report->cached_delta : 1;
          WriteModernZPDReport(*old_report, speculative);
          PERF_counter_inc(kZpdFastSpeculativeWrites);
        } else if (old_report) {
          AwaitModernZPDReport(old_handle, 2);
          BeginSubmission(true);
        }
      }
    }
    uint32_t slot_base = XenosZPDReport::GetSlotBase(address);
    bool same_slot_reuse = zpd_lifecycle_.HasPendingSlot(slot_base);
    if (same_slot_reuse && GetZPDMode() == ZPDMode::kStrict) {
      ZPDLifecycle::ReportHandle pending = zpd_lifecycle_.OldestPendingSlot(slot_base);
      if (pending != ZPDLifecycle::kInvalidReportHandle) {
        AwaitModernZPDReport(pending, 2);
        // Awaiting closes the current submission. Reopen before BeginQuery.
        BeginSubmission(true);
      }
    }
    std::memset(guest_report, 0, sizeof(*guest_report));
    auto begin = zpd_lifecycle_.Begin(address);
    if (begin.report_handle == ZPDLifecycle::kInvalidReportHandle) {
      PERF_counter_inc(kZpdMalformedRecords);
      return true;
    }
    PERF_counter_inc(kZpdReportsStarted);
    if (same_slot_reuse || begin.same_slot_reuse) {
      PERF_counter_inc(kZpdSameSlotReuse);
    }
    OpenModernZPDSegment();
    return true;
  }

  if (classification.classification == ZPDClassification::kMalformed) {
    if (XenosZPDReport::HasPendingSentinel(guest_report)) {
      PERF_counter_inc(kZpdFakeFallbacks);
      XenosZPDReport::WriteSampleCount(guest_report,
                                       uint32_t(REXCVAR_GET(query_occlusion_fake_sample_count)));
    }
    RecoverStalledZPDSentinel(address, guest_report);
    return true;
  }

  ZPDLifecycle::Report* active_report = zpd_lifecycle_.active_report();
  if (classification.classification == ZPDClassification::kOrphanedEnd || !active_report ||
      active_report->slot_base != XenosZPDReport::GetSlotBase(address)) {
    // Orphan END: never leave the guest polling the sentinel.
    uint32_t cached = zpd_lifecycle_.CachedDelta(record_base, 1);
    XenosZPDReport::WriteSampleCount(guest_report, cached);
    PERF_counter_inc(kZpdFastSpeculativeWrites);
    RecoverStalledZPDSentinel(address, guest_report);
    return true;
  }

  ZPDLifecycle::ReportHandle handle = zpd_lifecycle_.active().report_handle;
  CloseModernZPDSegment();
  zpd_lifecycle_.End(address);
  PERF_counter_inc(kZpdReportsEnded);

  active_report = zpd_lifecycle_.Find(handle);
  if (!active_report) {
    return true;
  }

  if (active_report->pending_segments == 0) {
    // Pool exhaustion or a failed segment open must still release guest
    // polling. Unknown visibility is conservatively treated as visible.
    WriteModernZPDReport(*active_report, 1);
    ZPDLifecycle::Report abandoned;
    bool commit = false;
    zpd_lifecycle_.Abandon(handle, 1, abandoned, commit);
    PERF_counter_inc(kZpdFakeFallbacks);
    return true;
  }

  if (GetZPDMode() == ZPDMode::kFast) {
    uint32_t speculative = active_report->has_cached_delta ? active_report->cached_delta : 1;
    WriteModernZPDReport(*active_report, speculative);
    PERF_counter_inc(kZpdFastSpeculativeWrites);
  } else {
    AwaitModernZPDReport(handle, 2);
  }
  RecoverStalledZPDSentinel(address, guest_report);
  return true;
}

bool D3D12CommandProcessor::PushTransitionBarrier(ID3D12Resource* resource,
                                                  D3D12_RESOURCE_STATES old_state,
                                                  D3D12_RESOURCE_STATES new_state,
                                                  UINT subresource) {
  if (old_state == new_state) {
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = old_state;
  barrier.Transition.StateAfter = new_state;
  barriers_.push_back(barrier);
  return true;
}

void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource,
                                                ID3D12Resource* new_resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Aliasing.pResourceBefore = old_resource;
  barrier.Aliasing.pResourceAfter = new_resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushUAVBarrier(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.UAV.pResource = resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::SubmitBarriers() {
  UINT barrier_count = UINT(barriers_.size());
  if (barrier_count != 0) {
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
    barriers_.clear();
  }
}

ID3D12RootSignature* D3D12CommandProcessor::GetRootSignature(const DxbcShader* vertex_shader,
                                                             const DxbcShader* pixel_shader,
                                                             bool tessellated) {
  if (bindless_resources_used_) {
    return tessellated ? root_signature_bindless_ds_ : root_signature_bindless_vs_;
  }

  D3D12_SHADER_VISIBILITY vertex_visibility =
      tessellated ? D3D12_SHADER_VISIBILITY_DOMAIN : D3D12_SHADER_VISIBILITY_VERTEX;

  uint32_t texture_count_vertex =
      uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t sampler_count_vertex =
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  uint32_t texture_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t sampler_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0;

  // Better put the pixel texture/sampler in the lower bits probably because it
  // changes often.
  uint32_t index = 0;
  uint32_t index_offset = 0;
  index |= texture_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= texture_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= uint32_t(vertex_visibility == D3D12_SHADER_VISIBILITY_DOMAIN) << index_offset;
  ++index_offset;
  assert_true(index_offset <= 32);

  // Try an existing root signature.
  auto it = root_signatures_bindful_.find(index);
  if (it != root_signatures_bindful_.end()) {
    return it->second;
  }

  // Create a new one.
  D3D12_ROOT_SIGNATURE_DESC desc;
  D3D12_ROOT_PARAMETER parameters[kRootParameter_Bindful_Count_Max];
  desc.NumParameters = kRootParameter_Bindful_Count_Base;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  // Base parameters.

  // Fetch constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FetchConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Vertex float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsVertex];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = vertex_visibility;
  }

  // Pixel float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsPixel];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  // System constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_SystemConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Bool and loop constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_BoolLoopConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Shared memory and, if ROVs are used, EDRAM.
  D3D12_DESCRIPTOR_RANGE shared_memory_and_edram_ranges[3];
  {
    auto& parameter = parameters[kRootParameter_Bindful_SharedMemoryAndEdram];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = shared_memory_and_edram_ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    shared_memory_and_edram_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shared_memory_and_edram_ranges[0].NumDescriptors = 1;
    shared_memory_and_edram_ranges[0].BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
    shared_memory_and_edram_ranges[0].RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    shared_memory_and_edram_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    shared_memory_and_edram_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    shared_memory_and_edram_ranges[1].NumDescriptors = 1;
    shared_memory_and_edram_ranges[1].BaseShaderRegister =
        UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
    shared_memory_and_edram_ranges[1].RegisterSpace = 0;
    shared_memory_and_edram_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
      ++parameter.DescriptorTable.NumDescriptorRanges;
      shared_memory_and_edram_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      shared_memory_and_edram_ranges[2].NumDescriptors = 1;
      shared_memory_and_edram_ranges[2].BaseShaderRegister =
          UINT(DxbcShaderTranslator::UAVRegister::kEdram);
      shared_memory_and_edram_ranges[2].RegisterSpace = 0;
      shared_memory_and_edram_ranges[2].OffsetInDescriptorsFromTableStart = 2;
    }
  }

  // Extra parameters.

  // Pixel textures.
  D3D12_DESCRIPTOR_RANGE range_textures_pixel;
  if (texture_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_textures_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_pixel.NumDescriptors = texture_count_pixel;
    range_textures_pixel.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_pixel.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Pixel samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_pixel;
  if (sampler_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_samplers_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_pixel.NumDescriptors = sampler_count_pixel;
    range_samplers_pixel.BaseShaderRegister = 0;
    range_samplers_pixel.RegisterSpace = 0;
    range_samplers_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex textures.
  D3D12_DESCRIPTOR_RANGE range_textures_vertex;
  if (texture_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_textures_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_vertex.NumDescriptors = texture_count_vertex;
    range_textures_vertex.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_vertex.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_vertex;
  if (sampler_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_samplers_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_vertex.NumDescriptors = sampler_count_vertex;
    range_samplers_vertex.BaseShaderRegister = 0;
    range_samplers_vertex.RegisterSpace = 0;
    range_samplers_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(GetD3D12Provider(), desc);
  if (root_signature == nullptr) {
    REXGPU_ERROR(
        "Failed to create a root signature with {} pixel textures, {} pixel "
        "samplers, {} vertex textures and {} vertex samplers",
        texture_count_pixel, sampler_count_pixel, texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  root_signatures_bindful_.emplace(index, root_signature);
  return root_signature;
}

uint32_t D3D12CommandProcessor::GetRootBindfulExtraParameterIndices(
    const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
    RootBindfulExtraParameterIndices& indices_out) {
  uint32_t index = kRootParameter_Bindful_Count_Base;
  if (pixel_shader && !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_pixel = index++;
  } else {
    indices_out.textures_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (pixel_shader && !pixel_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_pixel = index++;
  } else {
    indices_out.samplers_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_vertex = index++;
  } else {
    indices_out.textures_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_vertex = index++;
  } else {
    indices_out.samplers_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  return index;
}

uint64_t D3D12CommandProcessor::RequestViewBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = view_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = view_bindful_heap_pool_->GetLastRequestHeap();
  if (view_bindful_heap_current_ != heap) {
    view_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

uint32_t D3D12CommandProcessor::RequestPersistentViewBindlessDescriptor() {
  assert_true(bindless_resources_used_);
  if (!view_bindless_heap_free_.empty()) {
    uint32_t descriptor_index = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    return descriptor_index;
  }
  if (view_bindless_heap_allocated_ >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  return view_bindless_heap_allocated_++;
}

void D3D12CommandProcessor::ReleaseViewBindlessDescriptorImmediately(uint32_t descriptor_index) {
  assert_true(bindless_resources_used_);
  view_bindless_heap_free_.push_back(descriptor_index);
}

bool D3D12CommandProcessor::RequestOneUseSingleViewDescriptors(
    uint32_t count, ui::d3d12::util::DescriptorCpuGpuHandlePair* handles_out) {
  assert_true(submission_open_);
  if (!count) {
    return true;
  }
  assert_not_null(handles_out);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  if (bindless_resources_used_) {
    // Request separate bindless descriptors that will be freed when this
    // submission is completed by the GPU.
    if (count >
        kViewBindlessHeapSize - view_bindless_heap_allocated_ + view_bindless_heap_free_.size()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t descriptor_index;
      if (!view_bindless_heap_free_.empty()) {
        descriptor_index = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
      } else {
        descriptor_index = view_bindless_heap_allocated_++;
      }
      view_bindless_one_use_descriptors_.push_back(
          std::make_pair(descriptor_index, submission_current_));
      handles_out[i] = std::make_pair(
          provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, descriptor_index),
          provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, descriptor_index));
    }
  } else {
    // Request a range within the current heap for bindful resources path.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_start;
    if (RequestViewBindfulDescriptors(ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid, count,
                                      count, cpu_handle_start, gpu_handle_start) ==
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      handles_out[i] = std::make_pair(provider.OffsetViewDescriptor(cpu_handle_start, i),
                                      provider.OffsetViewDescriptor(gpu_handle_start, i));
    }
  }
  return true;
}

ui::d3d12::util::DescriptorCpuGpuHandlePair D3D12CommandProcessor::GetSystemBindlessViewHandlePair(
    SystemBindlessView view) const {
  assert_true(bindless_resources_used_);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  return std::make_pair(
      provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, uint32_t(view)),
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, uint32_t(view)));
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

uint64_t D3D12CommandProcessor::RequestSamplerBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = sampler_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = sampler_bindful_heap_pool_->GetLastRequestHeap();
  if (sampler_bindful_heap_current_ != heap) {
    sampler_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(uint32_t size,
                                                               D3D12_RESOURCE_STATES state) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || size == 0) {
    return nullptr;
  }

  if (size <= scratch_buffer_size_) {
    PushTransitionBarrier(scratch_buffer_, scratch_buffer_state_, state);
    scratch_buffer_state_ = state;
    scratch_buffer_used_ = true;
    return scratch_buffer_;
  }

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource* buffer;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                             state, nullptr, IID_PPV_ARGS(&buffer)))) {
    REXGPU_ERROR("Failed to create a {} MB scratch GPU buffer", size >> 20);
    return nullptr;
  }
  if (scratch_buffer_ != nullptr) {
    resources_for_deletion_.emplace_back(submission_current_, scratch_buffer_);
  }
  scratch_buffer_ = buffer;
  scratch_buffer_size_ = size;
  scratch_buffer_state_ = state;
  scratch_buffer_used_ = true;
  return scratch_buffer_;
}

void D3D12CommandProcessor::ReleaseScratchGPUBuffer(ID3D12Resource* buffer,
                                                    D3D12_RESOURCE_STATES new_state) {
  assert_true(submission_open_);
  assert_true(scratch_buffer_used_);
  scratch_buffer_used_ = false;
  if (buffer == scratch_buffer_) {
    scratch_buffer_state_ = new_state;
  }
}

// BEGIN FH1 OWNED GEOMETRY CACHE
auto D3D12CommandProcessor::FindFh1OwnedGeometry(uint32_t base, uint32_t size)
    -> std::map<uint64_t, Fh1Geometry>::iterator {
  const uint64_t key = (uint64_t(base) << 32) | size;
  if (!REXCVAR_GET(fh1_contain_geometry_windows)) return fh1_geometry_.find(key);
  // Choose the smallest containing owner. A later, larger import cannot switch
  // CPU bounds to a different resource while the caller holds this owner's GPU
  // address. Smaller requests already reuse it, so cannot insert a nearer key;
  // last_submission prevents eviction while the address is in flight.
  auto found = fh1_geometry_.lower_bound(key);
  return found != fh1_geometry_.end() && uint32_t(found->first >> 32) == base
      ? found : fh1_geometry_.end();
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12CommandProcessor::GetFh1OwnedGeometry(
    uint32_t address, uint32_t size, bool keep_cpu_snapshot) {
  const uint64_t budget = uint64_t(std::clamp(REXCVAR_GET(fh1_geometry_cache_mb), 8, 128)) << 20;
  if (!size || address >= SharedMemory::kBufferSize || size > SharedMemory::kBufferSize - address) {
    return 0;
  }
  // Share allocation-sized guest windows between nearby geometry ranges.
  // The view is 16-byte aligned; the fetch constant retains the low offset.
  const uint32_t view_offset = address & 0xFFF0u;
  const uint32_t end = (address + size + 0xFFFFu) & ~0xFFFFu;
  address &= ~0xFFFFu;
  size = end - address;
  const uint64_t key = (uint64_t(address) << 32) | size;
  auto found = FindFh1OwnedGeometry(address, size);
  if (found == fh1_geometry_.end()) {
    ++fh1_geometry_admission_attempts_;
    auto& rejection = fh1_geometry_rejections_[(key ^ (key >> 32)) % fh1_geometry_rejections_.size()];
    if (REXCVAR_GET(fh1_cache_geometry_rejections) && rejection.reason && rejection.key == key &&
        rejection.frame == frame_current_ &&
        rejection.completed_submission == submission_completed_ &&
        rejection.cache_generation == fh1_geometry_cache_generation_) {
      ++fh1_geometry_rejection_hits_;
      return 0;
    }
    auto reject = [&](uint8_t reason) {
      rejection = {key, frame_current_, submission_completed_, fh1_geometry_cache_generation_, reason};
      if (reason == 1) ++fh1_geometry_rejected_in_flight_;
      else ++fh1_geometry_rejected_recent_;
      return D3D12_GPU_VIRTUAL_ADDRESS(0);
    };
    D3D12_RESOURCE_DESC desc;
    ui::d3d12::util::FillBufferResourceDesc(desc, size, D3D12_RESOURCE_FLAG_NONE);
    const auto& provider = GetD3D12Provider();
    auto* device = provider.GetDevice();
    ++fh1_geometry_allocation_info_calls_;
    const uint64_t allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    if (allocation > budget) {
      return 0;
    }
    Fh1Geometry entry;
    // ponytail: linear eviction is bounded to 512 entries; no in-flight retirement
    // queue, so churn cannot exceed the geometry allocation budget.
    while (fh1_geometry_.size() >= 512 || fh1_geometry_bytes_ + allocation > budget) {
      ++fh1_geometry_eviction_scans_;
      auto oldest = std::min_element(fh1_geometry_.begin(), fh1_geometry_.end(),
          [](const auto& a, const auto& b) { return a.second.last_submission < b.second.last_submission; });
      if (REXCVAR_GET(fh1_recycle_geometry_buffers) && !entry.buffer.Get()) {
        auto fitting = fh1_geometry_.end();
        for (auto it = fh1_geometry_.begin(); it != fh1_geometry_.end(); ++it) {
          const auto& candidate = it->second;
          if (candidate.last_submission > submission_completed_ || uint32_t(it->first) != size ||
              candidate.allocation_bytes != allocation) continue;
          if (fitting == fh1_geometry_.end() ||
              candidate.last_submission < fitting->second.last_submission) fitting = it;
        }
        if (fitting != fh1_geometry_.end()) oldest = fitting;
      }
      if (oldest == fh1_geometry_.end() || oldest->second.last_submission > submission_completed_) {
        return reject(1);
      }
      // A scene larger than the cache must not recreate its working set every
      // frame. Keep recently used owners and use the shared-memory fallback.
      if (frame_current_ <= oldest->second.last_frame ||
          frame_current_ - oldest->second.last_frame <= 1) {
        return reject(2);
      }
      {
        auto lock = thread::global_critical_region::AcquireDirect();
        if (oldest->second.watch) shared_memory_->UnwatchMemoryRange(oldest->second.watch);
      }
      // Prefer matching completed storage without retaining oversized capacity.
      // The normal full-window import replaces all bytes and ownership metadata.
      if (REXCVAR_GET(fh1_recycle_geometry_buffers) && !entry.buffer.Get() &&
          uint32_t(oldest->first) == size && oldest->second.allocation_bytes == allocation) {
        entry.buffer = std::move(oldest->second.buffer);
        entry.state = oldest->second.state;
        ++fh1_geometry_recycles_;
      }
      fh1_geometry_bytes_ -= oldest->second.allocation_bytes;
      fh1_geometry_.erase(oldest);
      ++fh1_geometry_cache_generation_;
    }
    if (!entry.buffer.Get()) {
      if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
            provider.GetHeapFlagCreateNotZeroed(), &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&entry.buffer)))) {
        return 0;
      }
      ++fh1_geometry_allocations_;
    }
    entry.buffer->SetName((L"FH1 owned geometry " + std::to_wstring(address) + L" " +
                           std::to_wstring(size)).c_str());
    entry.allocation_bytes = allocation;
    found = fh1_geometry_.emplace(key, std::move(entry)).first;
    fh1_geometry_bytes_ += allocation;
    ++fh1_geometry_cache_generation_;
    fh1_geometry_peak_bytes_ = std::max(fh1_geometry_peak_bytes_, fh1_geometry_bytes_);
  }
  if (found->first != key) ++fh1_geometry_contained_uses_;
  // Watch, snapshot and import the entire selected owner, including bytes
  // outside a nested request that a later caller may consume.
  size = uint32_t(found->first);
  auto& entry = found->second;
  const bool add_snapshot = keep_cpu_snapshot && !entry.keep_cpu_snapshot;
  entry.keep_cpu_snapshot |= keep_cpu_snapshot;
  bool needs_import;
  {
    auto lock = thread::global_critical_region::AcquireDirect();
    needs_import = entry.watch == nullptr || add_snapshot;
    if (needs_import) {
      if (entry.watch) shared_memory_->UnwatchMemoryRange(entry.watch);
      // Arm before requesting/copying. A notification during import must stay
      // dirty for the next use, including writes made by the GPU.
      entry.watch = shared_memory_->WatchMemoryRange(address, size,
          [](const auto&, void*, void* data, uint64_t, bool) {
            static_cast<Fh1Geometry*>(data)->watch = nullptr;
          }, nullptr, &entry, 0);
    }
  }
  if (needs_import) {
    entry.depth_bounds.clear();
    entry.next_depth_bound = 0;
    entry.terrain_bounds.clear();
    entry.next_terrain_bound = 0;
    ID3D12Resource* source = nullptr;
    size_t source_offset = 0;
    uint8_t* upload = constant_buffer_pool_->Request(
        frame_current_, size, 16, &source, &source_offset, nullptr);
    bool from_cpu;
    if (entry.keep_cpu_snapshot) {
      entry.cpu_snapshot.resize(size);
      from_cpu = upload && shared_memory_->CopyCpuRange(address, entry.cpu_snapshot);
      if (from_cpu) std::memcpy(upload, entry.cpu_snapshot.data(), size);
      else entry.cpu_snapshot.clear();
    } else {
      from_cpu = upload && shared_memory_->CopyCpuRange(address, {upload, size});
    }
    if (!from_cpu && !shared_memory_->RequestRange(address, size)) {
      auto lock = thread::global_critical_region::AcquireDirect();
      if (entry.watch) shared_memory_->UnwatchMemoryRange(entry.watch);
      entry.watch = nullptr;
      return 0;
    }
    PushTransitionBarrier(entry.buffer.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!from_cpu) {
      shared_memory_->UseAsCopySource();
      source = shared_memory_->GetBuffer();
      source_offset = address;
    }
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(entry.buffer.Get(), 0, source, source_offset, size);
    PushTransitionBarrier(entry.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER));
    entry.state = (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER);
    if (from_cpu) {
      // This import may contain CPU writes absent from shared memory. A later
      // shared draw must request residency instead of reusing an older tag.
      for (uint32_t fetch : {89u, 90u, 94u, 95u}) {
        InvalidateVertexBufferResidency(fetch);
        vertex_buffer_states_[fetch] = {};
      }
      ++fh1_geometry_cpu_imports_;
    }
    fh1_geometry_import_bytes_ += size;
    if (++fh1_geometry_imports_ == 1) {
      REXGPU_INFO("FH1 owned geometry first import (bytes {}, CPU {})", size, from_cpu);
    }
  } else {
    if ((++fh1_geometry_hits_ & 0x3ffff) == 0) {
      REXGPU_INFO("FH1 owned geometry imports {}, CPU imports {}, cache hits {}, allocation bytes {}",
                  fh1_geometry_imports_, fh1_geometry_cpu_imports_, fh1_geometry_hits_, fh1_geometry_bytes_);
      REXGPU_INFO("FH1 geometry storage allocations {}, recycled {}",
                  fh1_geometry_allocations_, fh1_geometry_recycles_);
      REXGPU_INFO("FH1 geometry containment uses {}, import bytes {}",
                  fh1_geometry_contained_uses_, fh1_geometry_import_bytes_);
      REXGPU_INFO(
          "FH1 geometry admission attempts {}, allocation queries {}, eviction scans {}, "
          "rejection hits {}, in-flight rejects {}, recent rejects {}, peak bytes {}",
          fh1_geometry_admission_attempts_, fh1_geometry_allocation_info_calls_,
          fh1_geometry_eviction_scans_, fh1_geometry_rejection_hits_,
          fh1_geometry_rejected_in_flight_, fh1_geometry_rejected_recent_,
          fh1_geometry_peak_bytes_);
    }
  }
  entry.last_submission = submission_current_;
  entry.last_frame = frame_current_;
  return entry.buffer->GetGPUVirtualAddress() + view_offset;
}

std::span<const uint8_t> D3D12CommandProcessor::GetFh1OwnedGeometryCpuRange(
    uint32_t address, uint32_t size) {
  if (!size || address >= SharedMemory::kBufferSize ||
      size > SharedMemory::kBufferSize - address) return {};
  const uint32_t base = address & ~0xFFFFu;
  const uint32_t end = (address + size + 0xFFFFu) & ~0xFFFFu;
  auto found = FindFh1OwnedGeometry(base, end - base);
  if (found == fh1_geometry_.end() || found->second.cpu_snapshot.empty()) return {};
  // This is the import's immutable CPU copy, even if guest memory is now dirty.
  // A later import may replace it; callers revalidate after aliased imports.
  return std::span<const uint8_t>(found->second.cpu_snapshot).subspan(address - base, size);
}

std::optional<std::pair<uint32_t, uint32_t>> D3D12CommandProcessor::GetFh1DepthGeometryRange(
    uint32_t address, uint32_t size, std::span<const uint32_t, 8> system,
    uint32_t width, uint32_t stride, uint32_t fetch_address, uint32_t fetch_size,
    bool primitive_reset, uint32_t vertex_bytes) {
  if (!size || address >= SharedMemory::kBufferSize ||
      size > SharedMemory::kBufferSize - address) return {};
  const uint32_t base = address & ~0xFFFFu;
  const uint32_t end = (address + size + 0xFFFFu) & ~0xFFFFu;
  auto found = FindFh1OwnedGeometry(base, end - base);
  if (found == fh1_geometry_.end() || found->second.cpu_snapshot.empty()) return {};
  auto& entry = found->second;
  std::array<uint32_t, 16> key = {address, size, width, stride, fetch_address,
                                 fetch_size, uint32_t(primitive_reset), vertex_bytes};
  std::copy(system.begin(), system.end(), key.begin() + 8);
  for (const auto& bounds : entry.depth_bounds) {
    if (bounds.key == key) return bounds.range;
  }
  const auto range = depth_geometry_range(system, GetFh1OwnedGeometryCpuRange(address, size),
                                         width, stride, fetch_address, fetch_size, primitive_reset, vertex_bytes);
  // ponytail: at most 32 linear probes per window; measure before adding a hash table.
  if (entry.depth_bounds.empty()) entry.depth_bounds.reserve(32);
  if (entry.depth_bounds.size() < 32) {
    entry.depth_bounds.push_back({key, range});
  } else {
    entry.depth_bounds[entry.next_depth_bound] = {key, range};
    entry.next_depth_bound = (entry.next_depth_bound + 1) % 32;
  }
  return range;
}

std::optional<std::array<std::pair<uint32_t, uint32_t>, 3>>
D3D12CommandProcessor::GetFh1TerrainGeometryRanges(
    uint32_t address, uint32_t size, std::span<const uint32_t, 8> system,
    uint32_t width, bool primitive_reset, std::span<const float, 44> constants,
    std::span<const uint32_t, 6> fetches) {
  if (!size || address >= SharedMemory::kBufferSize ||
      size > SharedMemory::kBufferSize - address) return {};
  const uint32_t base = address & ~0xFFFFu;
  const uint32_t end = (address + size + 0xFFFFu) & ~0xFFFFu;
  auto found = FindFh1OwnedGeometry(base, end - base);
  if (found == fh1_geometry_.end() || found->second.cpu_snapshot.empty()) return {};
  auto& entry = found->second;
  std::array<uint32_t, 33> key = {address, size, width, uint32_t(primitive_reset)};
  std::copy(system.begin(), system.end(), key.begin() + 4);
  std::copy(fetches.begin(), fetches.end(), key.begin() + 12);
  // Matrices and unused constants do not affect resource bounds. Direction's
  // branch, rather than its arbitrary nonzero value, is the only relevant bit.
  constexpr uint32_t components[] = {16, 17, 18, 20, 21, 22, 28, 29, 30, 31, 32, 36, 37, 38, 39};
  for (uint32_t i = 0; i < 15; ++i) {
    if (components[i] == 32) key[18 + i] = constants[32] != 0;
    else std::memcpy(&key[18 + i], &constants[components[i]], sizeof(uint32_t));
  }
  for (const auto& bounds : entry.terrain_bounds) {
    if (bounds.key == key) return bounds.ranges;
  }
  // A different terrain tile can use the same immutable indices. Reuse that
  // maximum when only fetches or float bounds changed; imports clear both.
  const auto same_indices = std::find_if(entry.terrain_bounds.begin(), entry.terrain_bounds.end(),
      [&](const auto& bounds) { return std::equal(key.begin(), key.begin() + 12, bounds.key.begin()); });
  const auto maximum = same_indices != entry.terrain_bounds.end() ? same_indices->maximum :
      geometry_index_maximum(system, GetFh1OwnedGeometryCpuRange(address, size), width, primitive_reset);
  const auto ranges = maximum ? terrain_geometry_ranges(*maximum, constants, fetches) : std::nullopt;
  // ponytail: same bounded 32-probe policy as mesh depth; no second global cache.
  if (entry.terrain_bounds.empty()) entry.terrain_bounds.reserve(32);
  if (entry.terrain_bounds.size() < 32) {
    entry.terrain_bounds.push_back({key, maximum, ranges});
  } else {
    entry.terrain_bounds[entry.next_terrain_bound] = {key, maximum, ranges};
    entry.next_terrain_bound = (entry.next_terrain_bound + 1) % 32;
  }
  return ranges;
}

void D3D12CommandProcessor::ClearFh1OwnedGeometry() {
  {
    auto lock = thread::global_critical_region::AcquireDirect();
    for (auto& item : fh1_geometry_) {
      if (item.second.watch) shared_memory_->UnwatchMemoryRange(item.second.watch);
    }
  }
  fh1_geometry_.clear();
  fh1_geometry_bytes_ = 0;
  ++fh1_geometry_cache_generation_;
  fh1_geometry_rejections_ = {};
}
// END FH1 OWNED GEOMETRY CACHE

void D3D12CommandProcessor::SetExternalPipeline(ID3D12PipelineState* pipeline) {
  if (current_external_pipeline_ != pipeline) {
    current_external_pipeline_ = pipeline;
    current_guest_pipeline_ = nullptr;
    deferred_command_list_.D3DSetPipelineState(pipeline);
  }
}

void D3D12CommandProcessor::SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature) {
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }
  // Force-invalidate because setting a non-guest root signature.
  current_graphics_root_up_to_date_ = 0;
}

void D3D12CommandProcessor::SetViewport(const D3D12_VIEWPORT& viewport) {
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftX != viewport.TopLeftX;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftY != viewport.TopLeftY;
  ff_viewport_update_needed_ |= ff_viewport_.Width != viewport.Width;
  ff_viewport_update_needed_ |= ff_viewport_.Height != viewport.Height;
  ff_viewport_update_needed_ |= ff_viewport_.MinDepth != viewport.MinDepth;
  ff_viewport_update_needed_ |= ff_viewport_.MaxDepth != viewport.MaxDepth;
  if (ff_viewport_update_needed_) {
    ff_viewport_ = viewport;
    deferred_command_list_.RSSetViewport(ff_viewport_);
    ff_viewport_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetScissorRect(const D3D12_RECT& scissor_rect) {
  ff_scissor_update_needed_ |= ff_scissor_.left != scissor_rect.left;
  ff_scissor_update_needed_ |= ff_scissor_.top != scissor_rect.top;
  ff_scissor_update_needed_ |= ff_scissor_.right != scissor_rect.right;
  ff_scissor_update_needed_ |= ff_scissor_.bottom != scissor_rect.bottom;
  if (ff_scissor_update_needed_) {
    ff_scissor_ = scissor_rect;
    deferred_command_list_.RSSetScissorRect(ff_scissor_);
    ff_scissor_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetStencilReference(uint32_t stencil_ref) {
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  if (ff_stencil_ref_update_needed_) {
    ff_stencil_ref_ = stencil_ref;
    deferred_command_list_.D3DOMSetStencilRef(stencil_ref);
    ff_stencil_ref_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) {
  if (primitive_topology_ != primitive_topology) {
    primitive_topology_ = primitive_topology;
    deferred_command_list_.D3DIASetPrimitiveTopology(primitive_topology);
  }
}

std::string D3D12CommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Direct3D 12";
  if (render_target_cache_) {
    // Rasterizer-ordered views are a feature very rarely used as of 2020 and
    // that faces adoption complications (outside of Direct3D - on Vulkan - at
    // least), but crucial to Xenia - raise awareness of its usage.
    // https://github.com/KhronosGroup/Vulkan-Ecosystem/issues/27#issuecomment-455712319
    // "In Xenia's title bar "D3D12 ROV" can be seen, which was a surprise, as I
    //  wasn't aware that Xenia D3D12 backend was using Raster Order Views
    //  feature" - oscarbg in that issue.
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - RTV/DSV";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - ROV";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  return title.str();
}

bool D3D12CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  InvalidateAllVertexBufferResidency();
  UpdateDebugMarkersEnabled();

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  fence_completion_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_completion_event_ == nullptr) {
    REXGPU_ERROR("Failed to create the fence completion event");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission_fence_)))) {
    REXGPU_ERROR("Failed to create the submission fence");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&queue_operations_since_submission_fence_)))) {
    REXGPU_ERROR(
        "Failed to create the fence for awaiting queue operations done since "
        "the latest submission");
    return false;
  }

  // Create the command list and one allocator because it's needed for a command
  // list.
  ID3D12CommandAllocator* command_allocator;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&command_allocator)))) {
    REXGPU_ERROR("Failed to create a command allocator");
    return false;
  }
  command_allocator_writable_first_ = new CommandAllocator;
  command_allocator_writable_first_->command_allocator = command_allocator;
  command_allocator_writable_first_->last_usage_submission = 0;
  command_allocator_writable_first_->next = nullptr;
  command_allocator_writable_last_ = command_allocator_writable_first_;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator,
                                       nullptr, IID_PPV_ARGS(&command_list_)))) {
    REXGPU_ERROR("Failed to create the graphics command list");
    return false;
  }
  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));

  bindless_resources_used_ = REXCVAR_GET(d3d12_bindless) &&
                             provider.GetResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_2;

  // Get the draw resolution scale for the render target cache and the texture
  // cache.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, provider)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  shared_memory_ = std::make_unique<D3D12SharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }

  // Initialize the render target cache before configuring binding - need to
  // know if using rasterizer-ordered views for the bindless root signature.
  render_target_cache_ = std::make_unique<D3D12RenderTargetCache>(
      *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this,
      bindless_resources_used_);
  if (!render_target_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }

  // Initialize resource binding.
  constant_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                         sizeof(float) * 4 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
  if (bindless_resources_used_) {
    D3D12_DESCRIPTOR_HEAP_DESC view_bindless_heap_desc;
    view_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_bindless_heap_desc.NumDescriptors = kViewBindlessHeapSize;
    view_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    view_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&view_bindless_heap_desc,
                                            IID_PPV_ARGS(&view_bindless_heap_)))) {
      REXGPU_ERROR("Failed to create the bindless CBV/SRV/UAV descriptor heap");
      return false;
    }
    view_bindless_heap_cpu_start_ = view_bindless_heap_->GetCPUDescriptorHandleForHeapStart();
    view_bindless_heap_gpu_start_ = view_bindless_heap_->GetGPUDescriptorHandleForHeapStart();
    view_bindless_heap_allocated_ = uint32_t(SystemBindlessView::kCount);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_bindless_heap_desc;
    sampler_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_bindless_heap_desc.NumDescriptors = kSamplerHeapSize;
    sampler_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&sampler_bindless_heap_desc,
                                            IID_PPV_ARGS(&sampler_bindless_heap_current_)))) {
      REXGPU_ERROR("Failed to create the bindless sampler descriptor heap");
      return false;
    }
    sampler_bindless_heap_cpu_start_ =
        sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_gpu_start_ =
        sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_allocated_ = 0;
  } else {
    view_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewBindfulHeapSize);
    sampler_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  }

  if (bindless_resources_used_) {
    // Global bindless resource root signatures.
    // No CBV or UAV descriptor ranges with any descriptors to be allocated
    // dynamically (via RequestPersistentViewBindlessDescriptor or
    // RequestOneUseSingleViewDescriptors) should be here, because they would
    // overlap the unbounded SRV range, which is not allowed on Nvidia Fermi!
    D3D12_ROOT_SIGNATURE_DESC root_signature_bindless_desc;
    D3D12_ROOT_PARAMETER
    root_parameters_bindless[kRootParameter_Bindless_Count];
    root_signature_bindless_desc.NumParameters = kRootParameter_Bindless_Count;
    root_signature_bindless_desc.pParameters = root_parameters_bindless;
    root_signature_bindless_desc.NumStaticSamplers = 0;
    root_signature_bindless_desc.pStaticSamplers = nullptr;
    root_signature_bindless_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    // Fetch constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FetchConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Vertex float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // Pixel float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Pixel shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Vertex shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // System constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SystemConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Bool and loop constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_BoolLoopConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Shared memory SRV and UAV.
    D3D12_DESCRIPTOR_RANGE root_shared_memory_view_ranges[2];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SharedMemory];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges =
          uint32_t(rex::countof(root_shared_memory_view_ranges));
      parameter.DescriptorTable.pDescriptorRanges = root_shared_memory_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      {
        auto& range = root_shared_memory_view_ranges[0];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kMain);
        range.OffsetInDescriptorsFromTableStart = 0;
      }
      {
        auto& range = root_shared_memory_view_ranges[1];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 1;
      }
    }
    // Sampler heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_sampler_range;
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SamplerHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &root_bindless_sampler_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root_bindless_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      root_bindless_sampler_range.NumDescriptors = UINT_MAX;
      root_bindless_sampler_range.BaseShaderRegister = 0;
      root_bindless_sampler_range.RegisterSpace = 0;
      root_bindless_sampler_range.OffsetInDescriptorsFromTableStart = 0;
    }
    // View heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_view_ranges[4];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_ViewHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 0;
      parameter.DescriptorTable.pDescriptorRanges = root_bindless_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // EDRAM.
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kEdram);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kEdramR32UintUAV);
      }
      // Used UAV and SRV ranges must not overlap on Nvidia Fermi, so textures
      // have OffsetInDescriptorsFromTableStart after all static descriptors of
      // other types.
      // 2D array textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures2DArray);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // 3D textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures3D);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // Cube textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTexturesCube);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
    }
    root_signature_bindless_vs_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_vs_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use without tessellation");
      return false;
    }
    // The layered program uses one geometry SRV, two texture views and one sampler.
    // Keep constant slots compatible with UpdateBindings; reuse the unused pixel
    // descriptor-index slot for the signed texture table.
    D3D12_ROOT_PARAMETER layered_parameters[kRootParameter_Bindless_Count];
    std::copy(std::begin(root_parameters_bindless), std::end(root_parameters_bindless),
              std::begin(layered_parameters));
    auto& geometry_parameter = layered_parameters[kRootParameter_Bindless_SharedMemory];
    geometry_parameter = {};
    geometry_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    geometry_parameter.Descriptor.ShaderRegister = 0;
    geometry_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // Depth needs the same direct geometry SRV with the ordinary constant slots.
    auto depth_desc = root_signature_bindless_desc;
    depth_desc.pParameters = layered_parameters;
    root_signature_fh1_depth_ = ui::d3d12::util::CreateRootSignature(provider, depth_desc);
    // Skinned geometry keeps ordinary pixel tables and repurposes only the
    // unused vertex descriptor-index CBV for its second raw input.
    D3D12_ROOT_PARAMETER skinned_parameters[kRootParameter_Bindless_Count];
    std::copy(std::begin(layered_parameters), std::end(layered_parameters),
              std::begin(skinned_parameters));
    skinned_parameters[kRootParameter_Bindless_DescriptorIndicesVertex] = geometry_parameter;
    skinned_parameters[kRootParameter_Bindless_DescriptorIndicesVertex].Descriptor.ShaderRegister = 1;
    auto skinned_desc = depth_desc;
    skinned_desc.pParameters = skinned_parameters;
    root_signature_fh1_skinned_ = ui::d3d12::util::CreateRootSignature(provider, skinned_desc);
    // Terrain uses three raw vertex streams and no texture descriptor indices.
    D3D12_ROOT_PARAMETER terrain_parameters[kRootParameter_Bindless_Count];
    std::copy(std::begin(layered_parameters), std::end(layered_parameters),
              std::begin(terrain_parameters));
    const uint32_t terrain_slots[] = {kRootParameter_Bindless_DescriptorIndicesVertex,
                                      kRootParameter_Bindless_DescriptorIndicesPixel};
    for (uint32_t i = 0; i < 2; ++i) {
      auto& parameter = terrain_parameters[terrain_slots[i]];
      parameter = geometry_parameter;
      parameter.Descriptor.ShaderRegister = i + 1;
    }
    auto terrain_desc = depth_desc;
    terrain_desc.pParameters = terrain_parameters;
    root_signature_fh1_terrain_ = ui::d3d12::util::CreateRootSignature(provider, terrain_desc);
    D3D12_DESCRIPTOR_RANGE layered_ranges[3] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 1, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0}};
    const uint32_t layered_slots[] = {kRootParameter_Bindless_ViewHeap,
                                     kRootParameter_Bindless_DescriptorIndicesPixel,
                                     kRootParameter_Bindless_SamplerHeap};
    for (uint32_t i = 0; i < 3; ++i) {
      auto& parameter = layered_parameters[layered_slots[i]];
      parameter = {};
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable = {1, &layered_ranges[i]};
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    auto layered_desc = root_signature_bindless_desc;
    layered_desc.pParameters = layered_parameters;
    root_signature_fh1_layered_ = ui::d3d12::util::CreateRootSignature(provider, layered_desc);
    root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_signature_bindless_ds_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_ds_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use with tessellation");
      return false;
    }
  }

  primitive_processor_ =
      std::make_unique<D3D12PrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  texture_cache_ =
      D3D12TextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                draw_resolution_scale_y, *this, bindless_resources_used_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  pipeline_cache_ = std::make_unique<PipelineCache>(
      *this, *register_file_, *render_target_cache_.get(), bindless_resources_used_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed = provider.GetHeapFlagCreateNotZeroed();

  // Create gamma ramp resources.
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  D3D12_RESOURCE_DESC gamma_ramp_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(gamma_ramp_buffer_desc, (256 + 128 * 3) * 4,
                                          D3D12_RESOURCE_FLAG_NONE);
  // The first action will be uploading.
  gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             gamma_ramp_buffer_state_, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp buffer");
    return false;
  }
  // The upload buffer is frame-buffered.
  gamma_ramp_buffer_desc.Width *= kQueueFrames;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesUpload,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_upload_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
    return false;
  }
  if (FAILED(gamma_ramp_upload_buffer_->Map(
          0, nullptr, reinterpret_cast<void**>(&gamma_ramp_upload_buffer_mapping_)))) {
    REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
    gamma_ramp_upload_buffer_mapping_ = nullptr;
    return false;
  }

  // Initialize compute pipelines for output with gamma ramp.
  D3D12_ROOT_PARAMETER
  apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_constants =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    apply_gamma_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    apply_gamma_root_parameter_constants.Constants.ShaderRegister = 0;
    apply_gamma_root_parameter_constants.Constants.RegisterSpace = 0;
    apply_gamma_root_parameter_constants.Constants.Num32BitValues =
        sizeof(ApplyGammaConstants) / sizeof(uint32_t);
    apply_gamma_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_dest;
  apply_gamma_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  apply_gamma_root_descriptor_range_dest.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_dest.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_dest.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_dest =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kDestination)];
    apply_gamma_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_dest;
    apply_gamma_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_source;
  apply_gamma_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_source.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_source.BaseShaderRegister = 1;
  apply_gamma_root_descriptor_range_source.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_source =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kSource)];
    apply_gamma_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_source;
    apply_gamma_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_ramp;
  apply_gamma_root_descriptor_range_ramp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_ramp.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_ramp.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_ramp.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_ramp.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_gamma_ramp =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kRamp)];
    apply_gamma_root_parameter_gamma_ramp.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_ramp;
    apply_gamma_root_parameter_gamma_ramp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC apply_gamma_root_signature_desc;
  apply_gamma_root_signature_desc.NumParameters = UINT(ApplyGammaRootParameter::kCount);
  apply_gamma_root_signature_desc.pParameters = apply_gamma_root_parameters;
  apply_gamma_root_signature_desc.NumStaticSamplers = 0;
  apply_gamma_root_signature_desc.pStaticSamplers = nullptr;
  apply_gamma_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(apply_gamma_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, apply_gamma_root_signature_desc);
  if (!apply_gamma_root_signature_) {
    REXGPU_ERROR("Failed to create the gamma ramp application root signature");
    return false;
  }
  *(apply_gamma_table_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_table_cs, sizeof(shaders::apply_gamma_table_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline");
    return false;
  }
  *(apply_gamma_table_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_table_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_table_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with perceptual luma output");
    return false;
  }
  *(apply_gamma_pwl_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_pwl_cs, sizeof(shaders::apply_gamma_pwl_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_pipeline_) {
    REXGPU_ERROR("Failed to create the PWL gamma ramp application compute pipeline");
    return false;
  }
  *(apply_gamma_pwl_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_pwl_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "perceptual luma output");
    return false;
  }

  // Initialize compute pipelines for post-processing anti-aliasing.
  D3D12_ROOT_PARAMETER fxaa_root_parameters[UINT(FxaaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_constants =
        fxaa_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    fxaa_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaa_root_parameter_constants.Constants.ShaderRegister = 0;
    fxaa_root_parameter_constants.Constants.RegisterSpace = 0;
    fxaa_root_parameter_constants.Constants.Num32BitValues =
        sizeof(FxaaConstants) / sizeof(uint32_t);
    fxaa_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_dest;
  fxaa_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  fxaa_root_descriptor_range_dest.NumDescriptors = 1;
  fxaa_root_descriptor_range_dest.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_dest.RegisterSpace = 0;
  fxaa_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_dest =
        fxaa_root_parameters[UINT(FxaaRootParameter::kDestination)];
    fxaa_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_dest.DescriptorTable.pDescriptorRanges = &fxaa_root_descriptor_range_dest;
    fxaa_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_source;
  fxaa_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  fxaa_root_descriptor_range_source.NumDescriptors = 1;
  fxaa_root_descriptor_range_source.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_source.RegisterSpace = 0;
  fxaa_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_source =
        fxaa_root_parameters[UINT(FxaaRootParameter::kSource)];
    fxaa_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &fxaa_root_descriptor_range_source;
    fxaa_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC fxaa_root_sampler;
  fxaa_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  fxaa_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.MipLODBias = 0.0f;
  fxaa_root_sampler.MaxAnisotropy = 1;
  fxaa_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  fxaa_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  fxaa_root_sampler.MinLOD = 0.0f;
  fxaa_root_sampler.MaxLOD = 0.0f;
  fxaa_root_sampler.ShaderRegister = 0;
  fxaa_root_sampler.RegisterSpace = 0;
  fxaa_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC fxaa_root_signature_desc;
  fxaa_root_signature_desc.NumParameters = UINT(FxaaRootParameter::kCount);
  fxaa_root_signature_desc.pParameters = fxaa_root_parameters;
  fxaa_root_signature_desc.NumStaticSamplers = 1;
  fxaa_root_signature_desc.pStaticSamplers = &fxaa_root_sampler;
  fxaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(fxaa_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, fxaa_root_signature_desc);
  if (!fxaa_root_signature_) {
    REXGPU_ERROR("Failed to create the FXAA root signature");
    return false;
  }
  *(fxaa_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs), fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline");
    return false;
  }
  *(fxaa_extreme_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_extreme_cs, sizeof(shaders::fxaa_extreme_cs),
      fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the extreme-quality FXAA compute pipeline");
    return false;
  }

  if (bindless_resources_used_) {
    // Create the system bindless descriptors once all resources are
    // initialized.
    // kNullRawSRV.
    ui::d3d12::util::CreateBufferRawSRV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawSRV)),
        nullptr, 0);
    // kNullRawUAV.
    ui::d3d12::util::CreateBufferRawUAV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawUAV)),
        nullptr, 0);
    // kNullTexture2DArray.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
    null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    null_srv_desc.Texture2DArray.MostDetailedMip = 0;
    null_srv_desc.Texture2DArray.MipLevels = 1;
    null_srv_desc.Texture2DArray.FirstArraySlice = 0;
    null_srv_desc.Texture2DArray.ArraySize = 1;
    null_srv_desc.Texture2DArray.PlaneSlice = 0;
    null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture2DArray)));
    // kNullTexture3D.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    null_srv_desc.Texture3D.MostDetailedMip = 0;
    null_srv_desc.Texture3D.MipLevels = 1;
    null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture3D)));
    // kNullTextureCube.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    null_srv_desc.TextureCube.MostDetailedMip = 0;
    null_srv_desc.TextureCube.MipLevels = 1;
    null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTextureCube)));
    // kSharedMemoryRawSRV.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawSRV)));
    // kSharedMemoryR32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintSRV)),
        2);
    // kSharedMemoryR32G32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintSRV)),
        3);
    // kSharedMemoryR32G32B32A32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV)),
        4);
    // kSharedMemoryRawUAV.
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawUAV)));
    // kSharedMemoryR32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintUAV)),
        2);
    // kSharedMemoryR32G32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintUAV)),
        3);
    // kSharedMemoryR32G32B32A32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV)),
        4);
    // kEdramRawSRV.
    render_target_cache_->WriteEdramRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawSRV)));
    // kEdramR32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintSRV)),
        2);
    // kEdramR32G32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintSRV)),
        3);
    // kEdramR32G32B32A32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintSRV)),
        4);
    // kEdramRawUAV.
    render_target_cache_->WriteEdramRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawUAV)));
    // kEdramR32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintUAV)),
        2);
    // kEdramR32G32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintUAV)),
        3);
    // kEdramR32G32B32A32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintUAV)),
        4);
    // kGammaRampTableSRV.
    WriteGammaRampSRV(
        false, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                             uint32_t(SystemBindlessView::kGammaRampTableSRV)));
    // kGammaRampPWLSRV.
    WriteGammaRampSRV(
        true, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                            uint32_t(SystemBindlessView::kGammaRampPWLSRV)));
  }

  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();
  InitializeNativeGuestOutputGpuTiming();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

void D3D12CommandProcessor::ShutdownContext() {
  // Shutdown must drain submitted GPU work even after the worker is cancelled.
  AwaitAllQueueOperationsCompletion(true);
  InvalidateAllVertexBufferResidency();
  ShutdownNativeGuestOutputGpuTiming();
  ShutdownOcclusionQueryResources();
  ClearFh1OwnedGeometry();
  REXGPU_INFO("FH1 owned geometry imports {}, cache hits {}", fh1_geometry_imports_, fh1_geometry_hits_);
  REXGPU_INFO(
      "FH1 geometry admission attempts {}, allocation queries {}, eviction scans {}, "
      "rejection hits {}, in-flight rejects {}, recent rejects {}, peak bytes {}",
      fh1_geometry_admission_attempts_, fh1_geometry_allocation_info_calls_,
      fh1_geometry_eviction_scans_, fh1_geometry_rejection_hits_,
      fh1_geometry_rejected_in_flight_, fh1_geometry_rejected_recent_,
      fh1_geometry_peak_bytes_);

  ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
  scratch_buffer_size_ = 0;

  for (const std::pair<uint64_t, ID3D12Resource*>& resource_for_deletion :
       resources_for_deletion_) {
    resource_for_deletion.second->Release();
  }
  resources_for_deletion_.clear();

  fxaa_source_texture_submission_ = 0;
  fxaa_source_texture_.Reset();

  fh1_tonemap_pipeline_.Reset();
  fh1_tonemap_root_signature_.Reset();
  fh1_tonemap_render_target_format_ = DXGI_FORMAT_UNKNOWN;
  fh1_tonemap_native_draws_ = 0;
  fh1_velocity_dilate_pipeline_.Reset();
  fh1_velocity_dilate_root_signature_.Reset();
  fh1_velocity_dilate_render_target_format_ = DXGI_FORMAT_UNKNOWN;
  fh1_velocity_dilate_native_draws_ = 0;

  fxaa_extreme_pipeline_.Reset();
  fxaa_pipeline_.Reset();
  fxaa_root_signature_.Reset();
  apply_gamma_pwl_fxaa_luma_pipeline_.Reset();
  apply_gamma_pwl_pipeline_.Reset();
  apply_gamma_table_fxaa_luma_pipeline_.Reset();
  apply_gamma_table_pipeline_.Reset();
  apply_gamma_root_signature_.Reset();

  // Unmapping will be done implicitly by the destruction.
  gamma_ramp_upload_buffer_mapping_ = nullptr;
  gamma_ramp_upload_buffer_.Reset();
  gamma_ramp_buffer_.Reset();

  texture_cache_.reset();

  pipeline_cache_.reset();

  primitive_processor_.reset();

  // Shut down binding - bindless descriptors may be owned by subsystems like
  // the texture cache.

  // Root signatures are used by pipelines, thus freed after the pipelines.
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_ds_);
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_vs_);
  ui::d3d12::util::ReleaseAndNull(root_signature_fh1_layered_);
  ui::d3d12::util::ReleaseAndNull(root_signature_fh1_depth_);
  ui::d3d12::util::ReleaseAndNull(root_signature_fh1_skinned_);
  ui::d3d12::util::ReleaseAndNull(root_signature_fh1_terrain_);
  for (auto it : root_signatures_bindful_) {
    it.second->Release();
  }
  root_signatures_bindful_.clear();

  if (bindless_resources_used_) {
    texture_cache_bindless_sampler_map_.clear();
    for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
      sampler_bindless_heap_overflowed.first->Release();
    }
    sampler_bindless_heaps_overflowed_.clear();
    sampler_bindless_heap_allocated_ = 0;
    ui::d3d12::util::ReleaseAndNull(sampler_bindless_heap_current_);
    view_bindless_one_use_descriptors_.clear();
    view_bindless_heap_free_.clear();
    ui::d3d12::util::ReleaseAndNull(view_bindless_heap_);
  } else {
    sampler_bindful_heap_pool_.reset();
    view_bindful_heap_pool_.reset();
  }
  constant_buffer_pool_.reset();

  render_target_cache_.reset();

  shared_memory_.reset();

  deferred_command_list_.Reset();
  ui::d3d12::util::ReleaseAndNull(command_list_1_);
  ui::d3d12::util::ReleaseAndNull(command_list_);
  ClearCommandAllocatorCache();

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));

  // First release the fences since they may reference fence_completion_event_.

  queue_operations_done_since_submission_signal_ = false;
  queue_operations_since_submission_fence_last_ = 0;
  ui::d3d12::util::ReleaseAndNull(queue_operations_since_submission_fence_);

  ui::d3d12::util::ReleaseAndNull(submission_fence_);
  submission_open_ = false;
  submission_current_ = 1;
  submission_completed_ = 0;

  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }

  device_removed_ = false;

  CommandProcessor::ShutdownContext();
}

void D3D12CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
    InvalidateVertexBufferResidency((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2);
  }
}

void D3D12CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_bool_loop_.up_to_date = false;
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (graphics_system_->prepared_draw_observer()) {
      for (uint32_t index = start_index; index <= end_index; ++index) {
        observation_fetch_origins_[index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] = {
            observation_current_packet_address_, observation_indirect_buffer_execution_id_};
      }
    }
    cbuffer_binding_fetch_.up_to_date = false;
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void D3D12CommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

bool D3D12CommandProcessor::DrawFh1ToneMap(
    const D3D12Shader::TextureBinding& source,
    DXGI_FORMAT render_target_format, float exposure) {
  if (!bindless_resources_used_ || render_target_format == DXGI_FORMAT_UNKNOWN ||
      (fh1_tonemap_pipeline_ &&
       fh1_tonemap_render_target_format_ != render_target_format)) {
    return false;
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  if (!device) {
    return false;
  }
  if (!fh1_tonemap_root_signature_) {
    D3D12_DESCRIPTOR_RANGE source_range{};
    source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    source_range.NumDescriptors = 1;
    source_range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 1;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &source_range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = uint32_t(rex::countof(parameters));
    desc.pParameters = parameters;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    *fh1_tonemap_root_signature_.ReleaseAndGetAddressOf() =
        ui::d3d12::util::CreateRootSignature(provider, desc);
    if (!fh1_tonemap_root_signature_) {
      return false;
    }
  }
  if (!fh1_tonemap_pipeline_) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = fh1_tonemap_root_signature_.Get();
    desc.VS = {shaders::fh1_tonemap_vs, sizeof(shaders::fh1_tonemap_vs)};
    desc.PS = {shaders::fh1_tonemap_ps, sizeof(shaders::fh1_tonemap_ps)};
    auto& blend = desc.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = render_target_format;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(fh1_tonemap_pipeline_.ReleaseAndGetAddressOf())))) {
      return false;
    }
    fh1_tonemap_render_target_format_ = render_target_format;
  }

  const uint32_t source_index =
      texture_cache_->GetActiveTextureBindlessSRVIndex(source);
  if (source_index == UINT32_MAX) {
    return false;
  }
  const D3D12_GPU_DESCRIPTOR_HANDLE source_handle =
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, source_index);
  SubmitBarriers();
  SetExternalGraphicsRootSignature(fh1_tonemap_root_signature_.Get());
  deferred_command_list_.D3DSetGraphicsRoot32BitConstants(
      0, 1, &exposure, 0);
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(1, source_handle);
  SetExternalPipeline(fh1_tonemap_pipeline_.Get());
  SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  deferred_command_list_.D3DDrawInstanced(3, 1, 0, 0);
  return true;
}

bool D3D12CommandProcessor::DrawFh1VelocityDilate(
    const D3D12Shader::TextureBinding& source,
    DXGI_FORMAT render_target_format) {
  if (!bindless_resources_used_ || render_target_format == DXGI_FORMAT_UNKNOWN ||
      (fh1_velocity_dilate_pipeline_ &&
       fh1_velocity_dilate_render_target_format_ != render_target_format)) {
    return false;
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  if (!device) {
    return false;
  }
  if (!fh1_velocity_dilate_root_signature_) {
    D3D12_DESCRIPTOR_RANGE source_range{};
    source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    source_range.NumDescriptors = 1;
    source_range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &source_range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    *fh1_velocity_dilate_root_signature_.ReleaseAndGetAddressOf() =
        ui::d3d12::util::CreateRootSignature(provider, desc);
    if (!fh1_velocity_dilate_root_signature_) {
      return false;
    }
  }
  if (!fh1_velocity_dilate_pipeline_) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = fh1_velocity_dilate_root_signature_.Get();
    desc.VS = {shaders::fh1_tonemap_vs, sizeof(shaders::fh1_tonemap_vs)};
    desc.PS = {shaders::fh1_velocity_dilate_ps,
               sizeof(shaders::fh1_velocity_dilate_ps)};
    auto& blend = desc.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = render_target_format;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(
                       fh1_velocity_dilate_pipeline_.ReleaseAndGetAddressOf())))) {
      return false;
    }
    fh1_velocity_dilate_render_target_format_ = render_target_format;
  }

  const uint32_t source_index =
      texture_cache_->GetActiveTextureBindlessSRVIndex(source);
  if (source_index == UINT32_MAX) {
    return false;
  }
  const D3D12_GPU_DESCRIPTOR_HANDLE source_handle =
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, source_index);
  SubmitBarriers();
  SetExternalGraphicsRootSignature(fh1_velocity_dilate_root_signature_.Get());
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(0, source_handle);
  SetExternalPipeline(fh1_velocity_dilate_pipeline_.Get());
  SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  deferred_command_list_.D3DDrawInstanced(3, 1, 0, 0);
  return true;
}

void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  if (observation_frame_sequence_ % 600 == 0) {
    const auto& reflection_imports = texture_cache_->GetFh1ReflectionImportStats();
    REXGPU_INFO(
        "FH1 native reflection mips enabled={} candidates={} native_faces={} fallback_lists={} "
        "replaced_draws={} replaced_copies={} rejected_guard/state/contract/publication={}/{}/{}/{} "
        "cube_imports={} cube_direct_imports={} cube_subresource_copies={} cube_guest_bytes={} "
        "cube_upload_bytes={}",
        REXCVAR_GET(fh1_native_reflection_mips), fh1_mip_candidates_, fh1_mip_native_faces_,
        fh1_mip_candidates_ - fh1_mip_native_faces_, fh1_mip_native_faces_ * 8,
        fh1_mip_native_faces_ * 8, fh1_mip_rejections_[0], fh1_mip_rejections_[1],
        fh1_mip_rejections_[2], fh1_mip_rejections_[3], reflection_imports.loads,
        reflection_imports.direct_loads, reflection_imports.subresource_copies,
        reflection_imports.guest_bytes, reflection_imports.upload_bytes);
    if (REXCVAR_GET(fh1_mip_decode_probe) && fh1_mip_probe_faces_) {
      REXGPU_INFO(
          "FH1 mip decode probe {{\"faces\":{},\"contract_ns\":{},"
          "\"native_ns\":{},\"replay_ns\":{}}}",
          fh1_mip_probe_faces_, fh1_mip_probe_contract_ns_, fh1_mip_probe_native_ns_,
          fh1_mip_probe_replay_ns_);
    }
  }

  SCOPE_profile_cpu_f("gpu");
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;

  if (!graphics_system_)
    return;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("IssueSwap: presenter is null");
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("IssueSwap: BeginSubmission failed");
    return;
  }

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  D3D12_SHADER_RESOURCE_VIEW_DESC swap_texture_srv_desc;
  xenos::TextureFormat frontbuffer_format;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  ID3D12Resource* swap_texture_resource =
      texture_cache_->RequestSwapTexture(swap_texture_srv_desc, frontbuffer_format,
                                         &frontbuffer_width_unscaled, &frontbuffer_height_unscaled);
  if (!swap_texture_resource) {
    // Dump texture fetch constant 0 for debugging
    const auto& regs = *register_file_;
    auto fetch = regs.GetTextureFetch(0);
    REXGPU_ERROR(
        "IssueSwap: RequestSwapTexture failed - fetch0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    return;
  }
  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  uint32_t source_width_scaled = uint32_t(swap_texture_desc.Width);
  uint32_t source_height_scaled = uint32_t(swap_texture_desc.Height);
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width =
      get_active_swap_dimension(frontbuffer_width, frontbuffer_width_unscaled, source_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, source_height_scaled);
  if (!guest_output_width) {
    guest_output_width = source_width_scaled
                             ? source_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height = source_height_scaled ? source_height_scaled
                                               : (frontbuffer_height ? frontbuffer_height
                                                                     : frontbuffer_height_unscaled);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (source_width_scaled != frontbuffer_width_unscaled ||
                             source_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "D3D12 draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          guest_output_width, guest_output_height);
    }
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, &swap_texture_srv_desc, frontbuffer_format, swap_texture_resource, guest_output_width,
       guest_output_height, display_width,
       display_height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
        ID3D12Device* device = provider.GetDevice();

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;
        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc = fxaa_source_texture_->GetDesc();
            if (fxaa_source_texture_desc.Width != guest_output_width ||
                fxaa_source_texture_desc.Height != guest_output_height) {
              if (submission_completed_ < fxaa_source_texture_submission_) {
                fxaa_source_texture_->AddRef();
                resources_for_deletion_.emplace_back(fxaa_source_texture_submission_,
                                                     fxaa_source_texture_.Get());
              }
              fxaa_source_texture_.Reset();
              fxaa_source_texture_submission_ = 0;
            }
          }
          if (!fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc;
            fxaa_source_texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            fxaa_source_texture_desc.Alignment = 0;
            fxaa_source_texture_desc.Width = guest_output_width;
            fxaa_source_texture_desc.Height = guest_output_height;
            fxaa_source_texture_desc.DepthOrArraySize = 1;
            fxaa_source_texture_desc.MipLevels = 1;
            fxaa_source_texture_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_texture_desc.SampleDesc.Count = 1;
            fxaa_source_texture_desc.SampleDesc.Quality = 0;
            fxaa_source_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            fxaa_source_texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
                    &fxaa_source_texture_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nullptr, IID_PPV_ARGS(&fxaa_source_texture_)))) {
              REXGPU_ERROR("Failed to create the FXAA input texture");
              swap_post_effect = SwapPostEffect::kNone;
              use_fxaa = false;
            }
          }
        }

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Upload the new gamma ramp, using the upload buffer for the current
        // frame (will close the frame after this anyway, so can't write
        // multiple times per frame).
        if (!(use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                                 : gamma_ramp_256_entry_table_up_to_date_)) {
          uint32_t gamma_ramp_offset_bytes = use_pwl_gamma_ramp ? 256 * 4 : 0;
          uint32_t gamma_ramp_upload_offset_bytes =
              uint32_t(frame_current_ % kQueueFrames) * ((256 + 128 * 3) * 4) +
              gamma_ramp_offset_bytes;
          uint32_t gamma_ramp_size_bytes = (use_pwl_gamma_ramp ? 128 * 3 : 256) * 4;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload_buffer = reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_buffer_entry =
                  gamma_ramp_pwl_upload_buffer[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_buffer_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_buffer_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size_bytes);
          }
          PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                                D3D12_RESOURCE_STATE_COPY_DEST);
          gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              gamma_ramp_buffer_.Get(), gamma_ramp_offset_bytes, gamma_ramp_upload_buffer_.Get(),
              gamma_ramp_upload_offset_bytes, gamma_ramp_size_bytes);
          (use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                              : gamma_ramp_256_entry_table_up_to_date_) = true;
        }

        // Destination, source, and if bindful, gamma ramp.
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptors[3];
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptor_gamma_ramp;
        if (!RequestOneUseSingleViewDescriptors(bindless_resources_used_ ? 2 : 3,
                                                apply_gamma_descriptors)) {
          return false;
        }
        // Must not call anything that can change the descriptor heap from now
        // on!
        if (bindless_resources_used_) {
          apply_gamma_descriptor_gamma_ramp = GetSystemBindlessViewHandlePair(
              use_pwl_gamma_ramp ? SystemBindlessView::kGammaRampPWLSRV
                                 : SystemBindlessView::kGammaRampTableSRV);
        } else {
          apply_gamma_descriptor_gamma_ramp = apply_gamma_descriptors[2];
          WriteGammaRampSRV(use_pwl_gamma_ramp, apply_gamma_descriptor_gamma_ramp.first);
        }

        ID3D12Resource* guest_output_resource =
            static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
                .resource_uav_capable();

        if (use_fxaa) {
          fxaa_source_texture_submission_ = submission_current_;
        }

        ID3D12Resource* apply_gamma_dest =
            use_fxaa ? fxaa_source_texture_.Get() : guest_output_resource;
        D3D12_RESOURCE_STATES apply_gamma_dest_initial_state =
            use_fxaa ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                     : ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
            .resource_uav_capable();
        PushTransitionBarrier(apply_gamma_dest, apply_gamma_dest_initial_state,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // From now on, even in case of failure, apply_gamma_dest must be
        // transitioned back to apply_gamma_dest_initial_state!
        D3D12_UNORDERED_ACCESS_VIEW_DESC apply_gamma_dest_uav_desc;
        apply_gamma_dest_uav_desc.Format =
            use_fxaa ? kFxaaSourceTextureFormat : ui::d3d12::D3D12Presenter::kGuestOutputFormat;
        apply_gamma_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        apply_gamma_dest_uav_desc.Texture2D.MipSlice = 0;
        apply_gamma_dest_uav_desc.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(apply_gamma_dest, nullptr, &apply_gamma_dest_uav_desc,
                                          apply_gamma_descriptors[0].first);

        device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,
                                         apply_gamma_descriptors[1].first);

        PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        deferred_command_list_.D3DSetComputeRootSignature(apply_gamma_root_signature_.Get());
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = guest_output_width;
        apply_gamma_constants.size[1] = guest_output_height;
        deferred_command_list_.D3DSetComputeRoot32BitConstants(
            UINT(ApplyGammaRootParameter::kConstants),
            sizeof(apply_gamma_constants) / sizeof(uint32_t), &apply_gamma_constants, 0);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kDestination), apply_gamma_descriptors[0].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kSource), apply_gamma_descriptors[1].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kRamp), apply_gamma_descriptor_gamma_ramp.second);
        ID3D12PipelineState* apply_gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_pwl_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_pwl_pipeline_.Get();
        } else {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_table_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_table_pipeline_.Get();
        }
        SetExternalPipeline(apply_gamma_pipeline);
        SubmitBarriers();
        uint32_t group_count_x = (guest_output_width + 15) / 16;
        uint32_t group_count_y = (guest_output_height + 7) / 8;
        deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);

        // Apply FXAA.
        if (use_fxaa) {
          // Destination and source.
          ui::d3d12::util::DescriptorCpuGpuHandlePair fxaa_descriptors[2];
          if (!RequestOneUseSingleViewDescriptors(uint32_t(rex::countof(fxaa_descriptors)),
                                                  fxaa_descriptors)) {
            // Failed to obtain descriptors for FXAA - just copy after gamma
            // ramp application without applying FXAA.
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
            SubmitBarriers();
            deferred_command_list_.D3DCopyResource(guest_output_resource, apply_gamma_dest);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            return false;
          } else {
            assert_true(apply_gamma_dest_initial_state ==
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // From now on, even in case of failure, guest_output_resource must
            // be transitioned back to kGuestOutputInternalState!
            deferred_command_list_.D3DSetComputeRootSignature(fxaa_root_signature_.Get());
            FxaaConstants fxaa_constants;
            fxaa_constants.size[0] = guest_output_width;
            fxaa_constants.size[1] = guest_output_height;
            fxaa_constants.size_inv[0] = 1.0f / float(fxaa_constants.size[0]);
            fxaa_constants.size_inv[1] = 1.0f / float(fxaa_constants.size[1]);
            deferred_command_list_.D3DSetComputeRoot32BitConstants(
                UINT(FxaaRootParameter::kConstants), sizeof(fxaa_constants) / sizeof(uint32_t),
                &fxaa_constants, 0);
            D3D12_UNORDERED_ACCESS_VIEW_DESC fxaa_dest_uav_desc;
            fxaa_dest_uav_desc.Format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
            fxaa_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            fxaa_dest_uav_desc.Texture2D.MipSlice = 0;
            fxaa_dest_uav_desc.Texture2D.PlaneSlice = 0;
            device->CreateUnorderedAccessView(guest_output_resource, nullptr, &fxaa_dest_uav_desc,
                                              fxaa_descriptors[0].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kDestination), fxaa_descriptors[0].second);
            D3D12_SHADER_RESOURCE_VIEW_DESC fxaa_source_srv_desc;
            fxaa_source_srv_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fxaa_source_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fxaa_source_srv_desc.Texture2D.MostDetailedMip = 0;
            fxaa_source_srv_desc.Texture2D.MipLevels = 1;
            fxaa_source_srv_desc.Texture2D.PlaneSlice = 0;
            fxaa_source_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(fxaa_source_texture_.Get(), &fxaa_source_srv_desc,
                                             fxaa_descriptors[1].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kSource), fxaa_descriptors[1].second);
            SetExternalPipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                    ? fxaa_extreme_pipeline_.Get()
                                    : fxaa_pipeline_.Get());
            SubmitBarriers();
            deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          }
        } else {
          assert_true(apply_gamma_dest_initial_state ==
                      ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                apply_gamma_dest_initial_state);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue.
        SubmitBarriers();
        if (auto renderer = graphics_system_->native_guest_output_renderer().Get()) {
          system::NativeGuestOutputRenderContext native_context;
          native_context.backend = system::NativeGuestOutputBackend::kD3D12;
          native_context.guest_output_width = guest_output_width;
          native_context.guest_output_height = guest_output_height;
          native_context.display_width = display_width;
          native_context.display_height = display_height;
          native_context.output_format =
              uint32_t(ui::d3d12::D3D12Presenter::kGuestOutputFormat);
          native_context.device = device;
          native_context.command_context = this;
          native_context.guest_output = guest_output_resource;
          native_context.submission = submission_current_;
          native_context.frame_sequence = observation_frame_sequence_ - 1;
          native_context.use_pwl_gamma_ramp = use_pwl_gamma_ramp;
          native_context.xenos_fxaa_applied = use_fxaa;
          renderer(native_context);
        }
        SubmitBarriers();
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(d3d12_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* D3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const auto fh1_prepare_start =
      Fh1GpuCorpusEnabled() && Fh1ObserveCorpusFrame(observation_frame_sequence_) ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
  if (Fh1GpuCorpusEnabled()) {
    ++fh1_scene_draw_sequence_;
  }

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const RegisterFile& regs = *register_file_;
  const auto finish_draw = [](bool succeeded) { return succeeded; };

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return finish_draw(IssueCopy());
  }

  if (fh1_mip_replacement_active_) {
    ++fh1_mip_skipped_draws_;
    return true;
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  // Vertex shader analysis.
  auto vertex_shader = static_cast<D3D12Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return finish_draw(false);
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    return finish_draw(true);
  }
  D3D12Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<D3D12Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return finish_draw(true);
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  const bool fh1_constant_no_output =
      vertex_shader->ucode_data_hash() == 0xB6C9863F710683ECull &&
      pixel_shader && pixel_shader->ucode_data_hash() == 0xA4A965C189287B99ull &&
      !memexport_used && !active_occlusion_query_.valid;
  if (fh1_constant_no_output) {
    // These constant point draws have no guest-visible output. Keep query
    // draws and every writing state on the regular path.
    const auto no_output_depth = draw_util::GetNormalizedDepthControl(regs);
    if (!no_output_depth.z_enable && !no_output_depth.stencil_enable &&
        !draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())) {
      return finish_draw(true);
    }
  }

  if (!BeginSubmission(true)) {
    return finish_draw(false);
  }

  const uint64_t fh1_vertex_hash = vertex_shader->ucode_data_hash();
  const uint64_t fh1_pixel_hash =
      pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  const uint32_t fh1_depth_stride =
      fh1_vertex_hash == 0x9BF2991815B941B9ull ? 20 :
      fh1_vertex_hash == 0xC8C39E5AE1B08DE6ull ? 24 :
      fh1_vertex_hash == 0xB646F85EF69A57E0ull ? 28 :
      fh1_vertex_hash == 0xD0C40C04F166092Eull ? 32 : 0;
  const bool fh1_terrain_depth = fh1_vertex_hash == 0x5A28C7FAFD86F112ull ||
      fh1_vertex_hash == 0xCA293E0A1CB4B416ull || fh1_vertex_hash == 0x4E1DA281CC3D7EDBull;
  const uint32_t fh1_scene_stride =
      fh1_vertex_hash == 0xB8489164D5A86043ull && fh1_pixel_hash == 0x68150A8E959006CDull ? 32 :
      fh1_vertex_hash == 0xA3B9ED5D5C87230Eull ? 12 :
      fh1_vertex_hash == 0x6934E161812AB10Bull ? 28 :
      fh1_vertex_hash == 0xAD2C355A6BE1EE87ull && fh1_pixel_hash == 0x2F2137BF953DA7AFull ? 20 :
      fh1_vertex_hash == 0x8D8A197476841A9Aull && fh1_pixel_hash == 0xBA6A2871A980A4E8ull ? 16 : 0;
  const bool fh1_depth_indices = (((fh1_depth_stride || fh1_terrain_depth) && !pixel_shader) ||
      fh1_scene_stride) && !memexport_used &&
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  const bool fh1_native_velocity_candidate =
      fh1_vertex_hash == 0xC7D52884ECA9A039ull &&
      fh1_pixel_hash == 0xECE830AC0333767Full;
  const bool fh1_native_draw_candidate = fh1_native_velocity_candidate ||
      (fh1_vertex_hash == 0xA2FC50159EB69BA2ull &&
       fh1_pixel_hash == 0x618DD627F3D3B9A4ull);
  // FH1's fullscreen rectangle is auto-indexed with no conversion. Keep the
  // exact guest description for admission and fallback; native geometry is
  // supplied by SV_VertexID. Other backend expansion modes use Process.
  const bool native_primitive_prepared = fh1_native_velocity_candidate &&
      index_count == 3 && regs.Get<reg::VGT_DRAW_INITIATOR>().value == 0x00030088 &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().value == 0 &&
      !primitive_processor_->IsExpandingRectangleListsInVS();
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (native_primitive_prepared) {
    primitive_processing_result = {
        xenos::PrimitiveType::kRectangleList, xenos::PrimitiveType::kRectangleList,
        Shader::HostVertexShaderType::kVertex, regs.Get<reg::VGT_HOS_CNTL>().tess_mode,
        3, 3, 0, PrimitiveProcessor::ProcessedIndexBufferType::kNone, 0,
        xenos::IndexFormat::kInt16, xenos::Endian::kNone, false, SIZE_MAX};
  } else if (!primitive_processor_->Process(primitive_processing_result, fh1_depth_indices)) {
    return finish_draw(false);
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    return finish_draw(true);
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader
          ? pipeline_cache_->GetCurrentPixelShaderModification(
                *pixel_shader, interpolator_mask, ps_param_gen_pos, normalized_depth_control)
          : DxbcShaderTranslator::Modification(0);

  // Set up the render targets - this may perform dispatches and draws.
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  // A6's 2x depth-only design failed North Carson tail qualification.
  const bool owned_depth_clear = REXCVAR_GET(fh1_owned_depth_clear) &&
      texture_cache_->draw_resolution_scale_x() == 1 &&
      texture_cache_->draw_resolution_scale_y() == 1 &&
      !normalized_depth_control.stencil_enable;
  const bool owned_tile_clear = REXCVAR_GET(fh1_owned_depth_tile_clear) &&
      normalized_depth_control.stencil_enable &&
      !regs.Get<reg::RB_STENCILREFMASK>().stencilref &&
      (!normalized_depth_control.backface_enable ||
       !regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).stencilref);
  if ((owned_depth_clear || owned_tile_clear) && is_rasterization_done &&
      fh1_vertex_hash == 0x1E6883FCCDE1F688ull && !memexport_used &&
      !pixel_shader && !normalized_color_mask &&
      normalized_depth_control.z_enable && normalized_depth_control.z_write_enable &&
      regs.Get<reg::RB_SURFACE_INFO>().msaa_samples == xenos::MsaaSamples::k4X &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8 &&
      !active_occlusion_query_.valid && !zpd_lifecycle_.active_report() &&
      modern_occlusion_query_active_index_ == UINT32_MAX &&
      (index_count == 3 || index_count == 6) &&
      primitive_processing_result.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kNone &&
      primitive_processing_result.guest_primitive_type == xenos::PrimitiveType::kRectangleList &&
      primitive_processing_result.host_draw_vertex_count == index_count &&
      !regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable) {
    const auto try_owned_clear = [&]() {
      // Validate the same pipeline description as the established clear path,
      // without first asking Update to materialize its 4x target.
      auto* translation = static_cast<D3D12Shader::D3D12Translation*>(
          vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
      const uint32_t formats[1 + xenos::kMaxColorRenderTargets] = {
          uint32_t(xenos::DepthRenderTargetFormat::kD24S8)};
      void* clear_pipeline = nullptr;
      ID3D12RootSignature* clear_root = nullptr;
      if (!pipeline_cache_->ConfigurePipeline(translation, nullptr, primitive_processing_result,
          normalized_depth_control, 0, 1, formats, &clear_pipeline, &clear_root) ||
          !pipeline_cache_->IsFh1ClearPipeline(clear_pipeline)) return false;
      const uint32_t sx = texture_cache_->draw_resolution_scale_x();
      const uint32_t sy = texture_cache_->draw_resolution_scale_y();
      const bool convert = render_target_cache_->depth_float24_convert_in_pixel_shader();
      draw_util::ViewportInfo viewport;
      draw_util::GetHostViewportInfo(regs, sx, sy, true,
          D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
          normalized_depth_control, convert, true, false, viewport);
      draw_util::Scissor clip;
      draw_util::GetScissor(regs, clip);
      clip.offset[0] *= sx; clip.offset[1] *= sy;
      clip.extent[0] *= sx; clip.extent[1] *= sy;
      UpdateSystemConstantValues(false, primitive_polygonal,
          primitive_processing_result.line_loop_closing_index,
          primitive_processing_result.host_shader_index_endian, viewport, 0,
          normalized_depth_control, 0);
      std::array<uint32_t, 120> system{};
      static_assert(sizeof(system_constants_) <= sizeof(system));
      std::memcpy(system.data(), &system_constants_, sizeof(system_constants_));
      if ((system[0] & 1u) || system[4] || system[5] || system[6] ||
          system[7] < index_count - 1 || (system[3] && system[3] < index_count)) return false;
      const uint32_t fetch_address = regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
      if ((fetch_address & 3u) != 3u) return false;
      std::array<uint8_t, 168> bytes;
      if (!shared_memory_->CopyCpuSnapshot(fetch_address & ~3u,
          std::span<uint8_t>(bytes).first(index_count * 28))) return false;
      std::array<Fh1ClearRectangle, 2> rectangles;
      const uint32_t count = index_count / 3;
      for (uint32_t i = 0; i < count; ++i) {
        std::array<std::array<float, 4>, 3> vertices;
        for (uint32_t j = 0; j < 3; ++j) {
          const auto vertex = fh1_clear_vertex(
              std::span<const uint8_t, 28>(bytes.data() + (i * 3 + j) * 28, 28),
              regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1] & 3u, system, false);
          if (!vertex) return false;
          std::copy_n(vertex->begin(), 4, vertices[j].begin());
        }
        const auto rectangle = fh1_clear_rectangle(vertices,
            {viewport.xy_offset[0], viewport.xy_offset[1], viewport.xy_extent[0], viewport.xy_extent[1]},
            {viewport.z_min, viewport.z_max},
            {clip.offset[0], clip.offset[1], clip.extent[0], clip.extent[1]});
        if (!rectangle || (convert && rectangle->depth != 0)) return false;
        rectangles[i] = *rectangle;
      }
      const auto clear_rectangles = std::span<const Fh1ClearRectangle>(rectangles).first(count);
      return owned_tile_clear ? render_target_cache_->ClearFh1OwnedDepthTiles(clear_rectangles)
                              : render_target_cache_->ClearFh1OwnedDepth(clear_rectangles);
    };
    if (try_owned_clear()) return finish_draw(true);
  }
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return finish_draw(false);
  }

  // Obtain shader metadata and the actual render-target formats. Native passes
  // can validate this state without configuring a guest pipeline; fallback
  // still prepares the translations needed to obtain the used textures.
  D3D12Shader::D3D12Translation* vertex_shader_translation =
      static_cast<D3D12Shader::D3D12Translation*>(
          vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  D3D12Shader::D3D12Translation* pixel_shader_translation =
      pixel_shader ? static_cast<D3D12Shader::D3D12Translation*>(
                         pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                   : nullptr;
  uint32_t bound_depth_and_color_render_target_bits;
  uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets] = {};
  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  if (host_render_targets_used) {
    bound_depth_and_color_render_target_bits =
        render_target_cache_->GetLastUpdateBoundRenderTargets(
            bound_depth_and_color_render_target_formats);
  } else {
    bound_depth_and_color_render_target_bits = 0;
  }
  void* pipeline_handle = nullptr;
  ID3D12RootSignature* root_signature = nullptr;
  uint64_t pipeline_description_hash = 0;
  const auto configure_guest_pipeline = [&]() {
    return pipeline_cache_->ConfigurePipeline(
        vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
        normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,
        bound_depth_and_color_render_target_formats, &pipeline_handle, &root_signature);
  };
  const bool native_pipeline_description_ready =
      fh1_native_draw_candidate &&
      pipeline_cache_->GetNativeDrawPipelineDescriptionHash(
          vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
          normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats, pipeline_description_hash,
          fh1_native_velocity_candidate);
  if (!native_pipeline_description_ready) {
    if (!configure_guest_pipeline()) {
      return finish_draw(false);
    }
    if (REXCVAR_GET(async_shader_compilation) &&
        pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) == nullptr) {
      return finish_draw(true);
    }
    pipeline_description_hash = pipeline_cache_->GetPipelineDescriptionHash(pipeline_handle);
  }
  // This exact full-screen shader pair only samples fetch 3, including when
  // native admission later declines and the guest shaders are prepared lazily.
  uint32_t used_texture_mask = fh1_native_velocity_candidate
      ? (uint32_t(1) << 3)
      : vertex_shader->GetUsedTextureMaskAfterTranslation() |
            (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  auto prepared_draw_observer = graphics_system_->prepared_draw_observer();
  if (!Fh1ObserveCorpusFrame(observation_frame_sequence_)) {
    prepared_draw_observer = nullptr;
  }
  system::GraphicsPreparedDrawObservation prepared_observation;
  const auto get_fh1_attachment_state = [&]() {
    uint64_t state = 0xCBF29CE484222325ull;
    state = HashFh1ExecutionValue(
        state, regs.Get<reg::RB_SURFACE_INFO>().value);
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      state = HashFh1ExecutionValue(
          state, regs[reg::RB_COLOR_INFO::rt_register_indices[i]]);
    }
    state = HashFh1ExecutionValue(
        state, regs.Get<reg::RB_DEPTH_INFO>().value);
    state = HashFh1ExecutionValue(
        state, bound_depth_and_color_render_target_bits);
    for (uint32_t format : bound_depth_and_color_render_target_formats) {
      state = HashFh1ExecutionValue(state, format);
    }
    return state;
  };
  if (prepared_draw_observer || fh1_native_draw_candidate) {
    prepared_observation.frame_sequence = observation_frame_sequence_;
    prepared_observation.indirect_buffer_execution_id =
        observation_indirect_buffer_execution_id_;
    prepared_observation.indirect_buffer_parent_execution_id =
        observation_indirect_buffer_parent_execution_id_;
    prepared_observation.indirect_dispatch_packet_physical_address =
        observation_indirect_dispatch_packet_physical_address_;
    prepared_observation.draw_packet_physical_address =
        observation_draw_packet_address_;
    prepared_observation.command_buffer_physical_address =
        observation_draw_buffer_base_;
    prepared_observation.command_buffer_bytes =
        observation_draw_buffer_bytes_;
    prepared_observation.command_buffer_end_offset =
        observation_draw_buffer_end_offset_;
    prepared_observation.vertex_shader_hash = fh1_vertex_hash;
    prepared_observation.pixel_shader_hash = fh1_pixel_hash;
    prepared_observation.vertex_specialization_mask =
        vertex_shader_modification.value;
    prepared_observation.pixel_specialization_mask =
        pixel_shader_modification.value;
    prepared_observation.guest_primitive_type =
        uint32_t(primitive_processing_result.guest_primitive_type);
    prepared_observation.host_primitive_type =
        uint32_t(primitive_processing_result.host_primitive_type);
    prepared_observation.host_vertex_shader_type =
        uint32_t(primitive_processing_result.host_vertex_shader_type);
    prepared_observation.tessellation_mode =
        uint32_t(primitive_processing_result.tessellation_mode);
    prepared_observation.index_buffer_type =
        uint32_t(primitive_processing_result.index_buffer_type);
    prepared_observation.host_index_format =
        uint32_t(primitive_processing_result.host_index_format);
    prepared_observation.host_primitive_reset_enabled =
        primitive_processing_result.host_primitive_reset_enabled;
    prepared_observation.index_count = index_count;
    if (index_buffer_info) {
      prepared_observation.index_buffer_guest_base =
          index_buffer_info->guest_base;
      prepared_observation.index_buffer_length =
          uint32_t(index_buffer_info->length);
    }
    prepared_observation.normalized_depth_control =
        normalized_depth_control.value;
    prepared_observation.normalized_color_mask = normalized_color_mask;
    prepared_observation.bound_render_target_bits =
        bound_depth_and_color_render_target_bits;
    prepared_observation.surface_info = regs.Get<reg::RB_SURFACE_INFO>().value;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      prepared_observation.color_info[i] =
          regs[reg::RB_COLOR_INFO::rt_register_indices[i]];
    }
    prepared_observation.depth_info = regs.Get<reg::RB_DEPTH_INFO>().value;
    std::memcpy(prepared_observation.bound_render_target_formats,
                bound_depth_and_color_render_target_formats,
                sizeof(prepared_observation.bound_render_target_formats));
    prepared_observation.flags = uint32_t(host_render_targets_used) |
                                 (uint32_t(is_rasterization_done) << 1);
    auto& fh1_key = prepared_observation.fh1_execution_key;
    const auto append = [](uint64_t& hash, uint64_t value) {
      hash = HashFh1ExecutionValue(hash, value);
    };
    fh1_key.shader_state = 0xCBF29CE484222325ull;
    for (uint64_t value :
         {prepared_observation.vertex_shader_hash,
          prepared_observation.pixel_shader_hash,
          prepared_observation.vertex_specialization_mask,
          prepared_observation.pixel_specialization_mask,
          uint64_t(prepared_observation.host_vertex_shader_type),
          uint64_t(prepared_observation.tessellation_mode)}) {
      append(fh1_key.shader_state, value);
    }
    fh1_key.pipeline_state = pipeline_description_hash;
    fh1_key.attachment_state = get_fh1_attachment_state();
    fh1_key.resource_state = 0xCBF29CE484222325ull;
    append(fh1_key.resource_state, used_texture_mask);
    for (uint32_t fetch_index = 0; fetch_index < 32; ++fetch_index) {
      if (!(used_texture_mask & (uint32_t(1) << fetch_index))) {
        continue;
      }
      const auto fetch = regs.GetTextureFetch(fetch_index);
      uint32_t words[6];
      static_assert(sizeof(words) == sizeof(fetch));
      std::memcpy(words, &fetch, sizeof(words));
      // FH1 streams resource addresses while the binding layout stays stable.
      // ShiftGlue still resolves and validates the live resources; addresses
      // are not part of the title's prewarmed execution identity.
      words[1] &= 0xFFF;
      words[5] &= 0xFFF;
      append(fh1_key.resource_state, fetch_index);
      for (uint32_t word : words) {
        append(fh1_key.resource_state, word);
      }
    }
    for (const Shader::VertexBinding& binding :
         vertex_shader->vertex_bindings()) {
      const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
      uint32_t words[2];
      static_assert(sizeof(words) == sizeof(fetch));
      std::memcpy(words, &fetch, sizeof(words));
      append(fh1_key.resource_state, binding.fetch_constant);
      append(fh1_key.resource_state, binding.stride_words);
      append(fh1_key.resource_state, words[0] & 0x3);
      append(fh1_key.resource_state, words[1]);
    }
    if (index_buffer_info) {
      append(fh1_key.resource_state, index_buffer_info->length);
      append(fh1_key.resource_state, uint32_t(index_buffer_info->format));
      append(fh1_key.resource_state,
             uint32_t(index_buffer_info->endianness));
    }
    fh1_key.dynamic_state = 0xCBF29CE484222325ull;
    for (uint32_t register_index :
         {uint32_t(XE_GPU_REG_PA_CL_CLIP_CNTL),
          uint32_t(XE_GPU_REG_PA_CL_VTE_CNTL),
          uint32_t(XE_GPU_REG_PA_SU_SC_MODE_CNTL),
          uint32_t(XE_GPU_REG_PA_SU_VTX_CNTL),
          uint32_t(XE_GPU_REG_PA_SC_WINDOW_OFFSET),
          uint32_t(XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL),
          uint32_t(XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR),
          uint32_t(XE_GPU_REG_RB_BLEND_RED),
          uint32_t(XE_GPU_REG_RB_BLEND_GREEN),
          uint32_t(XE_GPU_REG_RB_BLEND_BLUE),
          uint32_t(XE_GPU_REG_RB_BLEND_ALPHA),
          uint32_t(XE_GPU_REG_RB_STENCILREFMASK),
          uint32_t(XE_GPU_REG_RB_STENCILREFMASK_BF)}) {
      append(fh1_key.dynamic_state, regs[register_index]);
    }
    for (uint32_t register_index = XE_GPU_REG_PA_CL_VPORT_XSCALE;
         register_index <= XE_GPU_REG_PA_CL_VPORT_ZOFFSET;
         ++register_index) {
      append(fh1_key.dynamic_state, regs[register_index]);
    }
    fh1_key.operation_state = 0xCBF29CE484222325ull;
    for (uint64_t value :
         {uint64_t(prepared_observation.guest_primitive_type),
          uint64_t(prepared_observation.host_primitive_type),
          uint64_t(index_count),
          uint64_t(prepared_observation.index_buffer_type),
          uint64_t(prepared_observation.host_index_format),
          uint64_t(prepared_observation.host_primitive_reset_enabled),
          uint64_t(major_mode_explicit)}) {
      append(fh1_key.operation_state, value);
    }
    fh1_key.hazard_flags =
        uint32_t(!vertex_shader->memexport_stream_constants().empty()) |
        (uint32_t(regs.Get<reg::PA_SC_VIZ_QUERY>().value != 0) << 1);
    fh1_key.flags = uint32_t(host_render_targets_used) |
                    (uint32_t(is_rasterization_done) << 1) |
                    (uint32_t(index_buffer_info != nullptr) << 2);
    fh1_key.identity = fh1_key.ComputeIdentity();
    // Diagnostic snapshots retain addresses and constants deliberately omitted
    // from prewarm identities. They are evidence, never batching admission.
    if (prepared_draw_observer && Fh1GpuCorpusEnabled() && Fh1SceneDumpEnabled() &&
        observation_frame_sequence_ >= 1200 &&
        observation_frame_sequence_ % 600 == 0 &&
        fh1_scene_binding_records_ < 4096 &&
        ((fh1_vertex_hash == 0xAD2C355A6BE1EE87ull &&
          fh1_pixel_hash == 0x2F2137BF953DA7AFull &&
          fh1_key.attachment_state == 0x7336ADFF531DCC00ull) ||
         fh1_scene_draw_sequence_ == fh1_scene_followup_sequence_)) {
      fh1_scene_followup_sequence_ =
          fh1_scene_draw_sequence_ == fh1_scene_followup_sequence_
              ? 0 : fh1_scene_draw_sequence_ + 1;
      std::string textures, vertices, constants;
      uint64_t command_hash = 0;
      if (observation_draw_buffer_bytes_ <= 8192 &&
          observation_draw_buffer_bytes_ % sizeof(uint32_t) == 0) {
        const auto* words = reinterpret_cast<const uint32_t*>(
            memory_->TranslatePhysical(observation_draw_buffer_base_));
        command_hash = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < observation_draw_buffer_bytes_ / 4; ++i) {
          command_hash = HashFh1ExecutionValue(command_hash, words[i]);
        }
        if (fh1_scene_command_snapshots_.size() < 32 &&
            fh1_scene_command_snapshots_.insert(command_hash).second) {
          std::string commands;
          for (uint32_t i = 0; i < observation_draw_buffer_bytes_ / 4; ++i) {
            commands += fmt::format("{:08X}", rex::byte_swap(words[i]));
          }
          REXGPU_INFO("FH1 scene commands {{\"hash\":\"{:016X}\","
                      "\"address\":{},\"words\":\"{}\"}}",
                      command_hash, observation_draw_buffer_base_, commands);
        }
      }
      for (uint32_t i = 0; i < 32; ++i) {
        if (!(used_texture_mask & (uint32_t(1) << i))) {
          continue;
        }
        const auto fetch = regs.GetTextureFetch(i);
        uint32_t words[6];
        std::memcpy(words, &fetch, sizeof(words));
        textures += fmt::format("{}:", i);
        for (uint32_t word : words) {
          textures += fmt::format("{:08X}", word);
        }
        textures += ';';
      }
      for (const auto& binding : vertex_shader->vertex_bindings()) {
        const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
        uint32_t words[2];
        std::memcpy(words, &fetch, sizeof(words));
        vertices += fmt::format("{}:{}:{:08X}{:08X};",
                                binding.fetch_constant, binding.stride_words,
                                words[0], words[1]);
      }
      for (uint32_t stage = 0; stage < 2; ++stage) {
        if (stage && !pixel_shader) {
          break;
        }
        const auto& map = (stage ? pixel_shader : vertex_shader)
                              ->constant_register_map();
        for (uint32_t i = 0; i < 256; ++i) {
          if (!(map.float_bitmap[i / 64] & (uint64_t(1) << (i % 64)))) {
            continue;
          }
          const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_000_X +
                             stage * 1024 + i * 4;
          constants += fmt::format("{}:{:08X}{:08X}{:08X}{:08X};", r,
                                   regs[r], regs[r + 1], regs[r + 2], regs[r + 3]);
        }
      }
      // Include the entire small bool/loop bank and constant base selectors.
      for (uint32_t r = XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031;
           r <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31; ++r) {
        constants += fmt::format("{}:{:08X};", r, regs[r]);
      }
      constants += fmt::format("bases:{:08X}{:08X};",
                               regs[XE_GPU_REG_SQ_VS_CONST],
                               regs[XE_GPU_REG_SQ_PS_CONST]);
      REXGPU_INFO(
          "FH1 scene binding {{\"frame\":{},\"sequence\":{},"
          "\"command_buffer\":{},\"command_bytes\":{},\"draw_end_offset\":{},"
          "\"command_hash\":\"{:016X}\","
          "\"scratch_mask\":{},\"scratch_address\":{},"
          "\"bin_mask\":\"{:016X}\",\"bin_select\":\"{:016X}\","
          "\"vertex_shader\":\"{:016X}\",\"pixel_shader\":\"{:016X}\","
          "\"attachment\":\"{:016X}\","
          "\"pipeline\":\"{:016X}\",\"dynamic\":\"{:016X}\","
          "\"operation\":\"{:016X}\",\"hazards\":{},\"flags\":{},"
          "\"index_base\":{},\"index_count\":{},\"index_length\":{},"
          "\"textures\":\"{}\",\"vertices\":\"{}\",\"constants\":\"{}\"}}",
          observation_frame_sequence_, fh1_scene_draw_sequence_,
          observation_draw_buffer_base_, observation_draw_buffer_bytes_,
          observation_draw_buffer_end_offset_,
          command_hash,
          regs[XE_GPU_REG_SCRATCH_UMSK], regs[XE_GPU_REG_SCRATCH_ADDR],
          bin_mask_, bin_select_,
          fh1_vertex_hash, fh1_pixel_hash, fh1_key.attachment_state,
          fh1_key.pipeline_state, fh1_key.dynamic_state, fh1_key.operation_state,
          fh1_key.hazard_flags, fh1_key.flags,
          prepared_observation.index_buffer_guest_base, index_count,
          prepared_observation.index_buffer_length, textures, vertices, constants);
      if (++fh1_scene_binding_records_ == 4096) {
        REXGPU_INFO("FH1 scene binding record limit reached");
      }
    }
    if (fh1_prepare_start != std::chrono::steady_clock::time_point{}) {
      prepared_observation.fh1_prepare_cpu_time_ns =
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - fh1_prepare_start)
                       .count());
    }
    ObserveFh1GpuPassTimingDraw(fh1_key,
                                prepared_observation.frame_sequence,
                                prepared_observation.fh1_prepare_cpu_time_ns);
    if (!pipeline_cache_->IsFh1PrewarmManifestLoaded()) {
      prepared_observation.fh1_fallback_reason =
          system::GraphicsFh1FallbackReason::kManifestUnavailable;
    } else if (fh1_key.hazard_flags) {
      prepared_observation.fh1_fallback_reason =
          system::GraphicsFh1FallbackReason::kHazardousDraw;
    } else if (!pipeline_cache_->IsFh1ExecutionCovered(fh1_key.identity)) {
      prepared_observation.fh1_fallback_reason =
          system::GraphicsFh1FallbackReason::kExecutionKeyNotCaptured;
    } else if (!pipeline_cache_->IsFh1PipelinePrewarmed(
                   fh1_key.pipeline_state)) {
      prepared_observation.fh1_fallback_reason =
          system::GraphicsFh1FallbackReason::kPipelineNotPrewarmed;
    } else {
      prepared_observation.fh1_execution_mode =
          system::GraphicsFh1ExecutionMode::kCoveredInPlace;
      prepared_observation.fh1_fallback_reason =
          system::GraphicsFh1FallbackReason::kNone;
    }
    prepared_observation.fh1_runtime_shader_translations =
        pipeline_cache_->GetFh1RuntimeShaderTranslationCount();
    prepared_observation.fh1_runtime_sync_pipeline_creations =
        pipeline_cache_->GetFh1RuntimeSyncPipelineCreationCount();
    if (prepared_draw_observer) {
      std::array<system::GraphicsPreparedDrawVertexFetch, 8> vertex_fetches;
      for (const auto& binding : vertex_shader->vertex_bindings()) {
        if (prepared_observation.vertex_fetch_count < vertex_fetches.size()) {
          const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
          const uint32_t origin_index = binding.fetch_constant * 2;
          const auto& source_0 = observation_fetch_origins_[origin_index];
          const auto& source_1 = observation_fetch_origins_[origin_index + 1];
          vertex_fetches[prepared_observation.vertex_fetch_count] = {
              binding.fetch_constant, binding.stride_words,
              uint32_t(fetch.address) << 2, uint32_t(fetch.size) << 2,
              uint32_t(fetch.type), source_0.packet_physical,
              source_1.packet_physical, source_0.execution_id,
              source_1.execution_id};
        }
        ++prepared_observation.vertex_fetch_count;
      }
      prepared_observation.vertex_fetches = vertex_fetches.data();
      prepared_observation.vertex_fetch_capacity = uint32_t(vertex_fetches.size());
      prepared_draw_observer(prepared_observation);
    }
  }

  const D3D12Shader::TextureBinding* fh1_tonemap_source = nullptr;
  DXGI_FORMAT fh1_tonemap_target_format = DXGI_FORMAT_UNKNOWN;
  if (prepared_observation.fh1_execution_key.identity ==
          0xA20B16064DBF09DFull &&
      prepared_observation.fh1_execution_mode ==
          system::GraphicsFh1ExecutionMode::kCoveredInPlace &&
      vertex_shader->ucode_data_hash() == 0xA2FC50159EB69BA2ull &&
      pixel_shader &&
      pixel_shader->ucode_data_hash() == 0x618DD627F3D3B9A4ull &&
      index_count == 24 && !memexport_used && host_render_targets_used &&
      is_rasterization_done &&
      regs.Get<reg::RB_SURFACE_INFO>().msaa_samples ==
          xenos::MsaaSamples::k1X &&
      (bound_depth_and_color_render_target_bits & 2)) {
    const auto& vertex_bindings = vertex_shader->vertex_bindings();
    if (vertex_bindings.size() == 1 &&
        vertex_bindings.front().fetch_constant == 95 &&
        vertex_bindings.front().stride_words == 4) {
      const auto fetch = regs.GetVertexFetch(95);
      const uint32_t address = fetch.address << 2;
      const uint32_t size = fetch.size << 2;
      const uint32_t* words = reinterpret_cast<const uint32_t*>(
          memory_->TranslatePhysical(address));
      uint64_t geometry = 0xCBF29CE484222325ull;
      if (address == 0x132C7FA0 && size == 384 &&
          fetch.endian == xenos::Endian::k8in32 && words) {
        for (uint32_t i = 0; i < size / sizeof(uint32_t); ++i) {
          geometry = HashFh1ExecutionValue(geometry, words[i]);
        }
      }
      if (geometry == 0xB3B603527C3B8E5Bull) {
        const auto& texture_bindings =
            pixel_shader->GetTextureBindingsAfterTranslation();
        for (const auto& binding : texture_bindings) {
          if (binding.fetch_constant == 0 &&
              binding.dimension == xenos::FetchOpDimension::k2D &&
              !binding.is_signed) {
            if (fh1_tonemap_source) {
              fh1_tonemap_source = nullptr;
              break;
            }
            fh1_tonemap_source = &binding;
          }
        }
        if (fh1_tonemap_source) {
          fh1_tonemap_target_format = DXGI_FORMAT(
              bound_depth_and_color_render_target_formats[1]);
        }
      }
    }
  }

  const D3D12Shader::TextureBinding* fh1_velocity_dilate_source = nullptr;
  DXGI_FORMAT fh1_velocity_dilate_target_format = DXGI_FORMAT_UNKNOWN;
  auto& fh1_key = prepared_observation.fh1_execution_key;
  // Native admission is fully specified below, independently of the guest
  // prewarm catalog. The family hash added no check beyond these three keys.
  if (fh1_key.shader_state == 0xE4ED1EE434D8FCE7ull &&
      fh1_key.pipeline_state == 0x6E456C111D3FA84Dull &&
      fh1_key.attachment_state == 0xBD706BD142DBDE84ull &&
      fh1_key.resource_state == 0x31081AC1C2066568ull &&
      fh1_key.operation_state == 0xB5D205630ADE9126ull &&
      fh1_key.hazard_flags == 0 && fh1_key.flags == 3 &&
      vertex_shader->ucode_data_hash() == 0xC7D52884ECA9A039ull &&
      pixel_shader &&
      pixel_shader->ucode_data_hash() == 0xECE830AC0333767Full &&
      used_texture_mask == (uint32_t(1) << 3) && index_count == 3 &&
      !memexport_used && host_render_targets_used && is_rasterization_done &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14000500 &&
      bound_depth_and_color_render_target_bits == 2 &&
      bound_depth_and_color_render_target_formats[1] ==
          uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) &&
      regs[XE_GPU_REG_SHADER_CONSTANT_255_X] == 0x3F000000 &&
      regs[XE_GPU_REG_SHADER_CONSTANT_255_Y] == 0xBF000000 &&
      regs[XE_GPU_REG_SHADER_CONSTANT_255_Z] == 0 &&
      regs[XE_GPU_REG_SHADER_CONSTANT_255_W] == 0 &&
      regs[XE_GPU_REG_SHADER_CONSTANT_511_X] == 0x3C23D70A &&
      regs[XE_GPU_REG_SHADER_CONSTANT_511_Y] == 0 &&
      regs[XE_GPU_REG_SHADER_CONSTANT_511_Z] == 0 &&
      regs[XE_GPU_REG_SHADER_CONSTANT_511_W] == 0) {
    const auto& vertex_bindings = vertex_shader->vertex_bindings();
    const auto vertex_fetch = regs.GetVertexFetch(95);
    if (vertex_bindings.size() == 1 &&
        vertex_bindings.front().fetch_constant == 95 &&
        vertex_bindings.front().stride_words == 2 &&
        vertex_fetch.endian == xenos::Endian::k8in32) {
      // The exact shader pair above samples fetch 3 as unsigned 2D. Its
      // native binding does not need the translated shader's descriptor table.
      // Live resource validity and signs are still checked by the texture cache.
      static constexpr D3D12Shader::TextureBinding native_source{
          0, 3, xenos::FetchOpDimension::k2D, false};
      fh1_velocity_dilate_source = &native_source;
      fh1_velocity_dilate_target_format =
          render_target_cache_->GetColorDrawDXGIFormat(
              xenos::ColorRenderTargetFormat(
                  bound_depth_and_color_render_target_formats[1]));
    }
  }

  // Update the textures - this may bind pipelines.
  // Qualified at 1x/2x; unsupported texture layouts retain the normal load path.
  const bool fh1_video_textures = pixel_shader &&
      (texture_cache_->draw_resolution_scale_x() == 1 ||
       texture_cache_->draw_resolution_scale_x() == 2) &&
      texture_cache_->draw_resolution_scale_y() == texture_cache_->draw_resolution_scale_x() &&
      vertex_shader->ucode_data_hash() == 0x7156CE05C6365E51ull &&
      pixel_shader->ucode_data_hash() == 0x31511D87CC0C94B9ull;
  const bool time_texture_request = Fh1GpuCorpusEnabled() &&
      observation_frame_sequence_ % 60 == 0;
  const auto texture_request_start = time_texture_request
      ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  if (fh1_video_textures) {
    texture_cache_->RequestFh1VideoTextures(used_texture_mask);
  } else if (root_signature_fh1_layered_ && root_signature == root_signature_fh1_layered_) {
    texture_cache_->RequestFh1Textures(used_texture_mask);
  } else {
    texture_cache_->RequestTextures(used_texture_mask);
  }
  if (time_texture_request) {
    PERF_counter_add(kTextureRequestCpuTimeNs,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - texture_request_start).count());
    PERF_counter_inc(kTextureRequestTimingSamples);
  }
  if (fh1_velocity_dilate_source &&
      texture_cache_->GetActiveTextureSwizzledSigns(3) != 0) {
    fh1_velocity_dilate_source = nullptr;
  }


  // Bind the pipeline after configuring it and doing everything that may bind
  // other pipelines.
  const auto bind_prepared_guest_pipeline = [this, &pipeline_handle]() {
    if (current_guest_pipeline_ != pipeline_handle) {
      deferred_command_list_.SetPipelineStateHandle(
          reinterpret_cast<void*>(pipeline_handle));
      current_guest_pipeline_ = pipeline_handle;
      current_external_pipeline_ = nullptr;
    }
  };

  // Get dynamic rasterizer state.
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  bool convert_z_to_float24 =
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader();
  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();

  // Build a cache key from all viewport-affecting state to skip redundant
  // recalculation when the viewport registers haven't changed between draws.
  ViewportCacheKey viewport_key;
  viewport_key.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  viewport_key.pa_cl_vte_cntl = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
  viewport_key.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  viewport_key.pa_su_vtx_cntl = regs[XE_GPU_REG_PA_SU_VTX_CNTL];
  viewport_key.pa_sc_window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  viewport_key.normalized_depth_control = normalized_depth_control.value;
  std::memcpy(viewport_key.vport_regs, &regs[XE_GPU_REG_PA_CL_VPORT_XSCALE],
              sizeof(viewport_key.vport_regs));
  viewport_key.flags = (uint32_t(convert_z_to_float24) << 0) |
                       (uint32_t(host_render_targets_used) << 1) | (uint32_t(ps_writes_depth) << 2);

  draw_util::ViewportInfo viewport_info;
  if (viewport_cache_valid_ && viewport_key == previous_viewport_key_) {
    viewport_info = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x, draw_resolution_scale_y, true,
                                   D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
                                   normalized_depth_control, convert_z_to_float24,
                                   host_render_targets_used, ps_writes_depth, viewport_info);
    previous_viewport_key_ = viewport_key;
    previous_viewport_info_ = viewport_info;
    viewport_cache_valid_ = true;
  }

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // Update viewport, scissor, blend factor and stencil reference.
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control);

  // Native full-screen draws use their own roots and SV_VertexID geometry.
  // Submit before guest constant uploads and vertex-buffer residency work.
  if (fh1_velocity_dilate_source) {
    deferred_command_list_.BeginDebugMarker(
        "PinyonShift V5 native FH1 velocity dilation");
    const bool native_velocity_dilate_drawn = DrawFh1VelocityDilate(
        *fh1_velocity_dilate_source, fh1_velocity_dilate_target_format);
    deferred_command_list_.EndDebugMarker();
    if (native_velocity_dilate_drawn) {
      ++fh1_velocity_dilate_native_draws_;
      if (fh1_velocity_dilate_native_draws_ == 1) {
        REXGPU_INFO(
            "FH1 V5 native velocity-dilate draws 1 (guest VS translated {}, PS translated {}, prewarm manifest {})",
            vertex_shader_translation->is_translated(), pixel_shader_translation->is_translated(),
            pipeline_cache_->IsFh1PrewarmManifestLoaded());
        REXGPU_INFO(
            "FH1 native velocity primitive initiator {:08X}, output path {:08X}, "
            "guest {}, host {}, index buffer {}, vertices {}, native preparation {}",
            regs.Get<reg::VGT_DRAW_INITIATOR>().value,
            regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().value,
            uint32_t(primitive_processing_result.guest_primitive_type),
            uint32_t(primitive_processing_result.host_primitive_type),
            uint32_t(primitive_processing_result.index_buffer_type),
            primitive_processing_result.host_draw_vertex_count, native_primitive_prepared);
      }
      return finish_draw(true);
    }
  }
  // Update system constants before uploading them.
  // TODO(Triang3l): With ROV, pass the disabled render target mask for safety.
  UpdateSystemConstantValues(memexport_used, primitive_polygonal,
                             primitive_processing_result.line_loop_closing_index,
                             primitive_processing_result.host_shader_index_endian, viewport_info,
                             used_texture_mask, normalized_depth_control, normalized_color_mask);

  // A qualified clear replaces rasterization, not render-target ownership or
  // transfer preparation. Reject query draws and non-CPU-authoritative input.
  if (fh1_vertex_hash == 0x1E6883FCCDE1F688ull && !memexport_used &&
      !active_occlusion_query_.valid && !zpd_lifecycle_.active_report() &&
      modern_occlusion_query_active_index_ == UINT32_MAX &&
      (index_count == 3 || index_count == 6) &&
      primitive_processing_result.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kNone &&
      primitive_processing_result.guest_primitive_type == xenos::PrimitiveType::kRectangleList &&
      primitive_processing_result.host_draw_vertex_count == index_count &&
      !regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable &&
      pipeline_cache_->IsFh1ClearPipeline(pipeline_handle)) {
    const auto try_clear = [&]() {
      std::array<uint32_t, 120> system{};
      static_assert(sizeof(system_constants_) <= sizeof(system));
      std::memcpy(system.data(), &system_constants_, sizeof(system_constants_));
      if ((system[0] & 1u) || system[4] || system[5] || system[6] ||
          system[7] < index_count - 1 || (system[3] && system[3] < index_count)) return false;
      if (normalized_color_mask && normalized_color_mask != 15) return false;
      if (normalized_depth_control.backface_enable && normalized_depth_control.stencil_enable &&
          regs.Get<reg::RB_STENCILREFMASK>().stencilref !=
          regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).stencilref) return false;
      const uint32_t fetch_address = regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
      const uint32_t fetch_size = regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1];
      if ((fetch_address & 3u) != 3u) return false;
      std::array<uint8_t, 168> bytes;
      if (!shared_memory_->CopyCpuSnapshot(fetch_address & ~3u,
          std::span<uint8_t>(bytes).first(index_count * 28))) return false;
      std::array<Fh1ClearRectangle, 2> rectangles;
      std::array<std::array<float, 4>, 2> colors{};
      const uint32_t count = index_count / 3;
      for (uint32_t i = 0; i < count; ++i) {
        std::array<std::array<float, 4>, 3> vertices;
        for (uint32_t j = 0; j < 3; ++j) {
          const auto vertex = fh1_clear_vertex(
              std::span<const uint8_t, 28>(bytes.data() + (i * 3 + j) * 28, 28),
              fetch_size & 3u, system, pixel_shader != nullptr);
          if (!vertex) return false;
          std::copy_n(vertex->begin(), 4, vertices[j].begin());
          if (!j) std::copy_n(vertex->begin() + 4, 4, colors[i].begin());
          else if (std::memcmp(colors[i].data(), vertex->data() + 4, 16)) return false;
        }
        auto rectangle = fh1_clear_rectangle(vertices,
            {viewport_info.xy_offset[0], viewport_info.xy_offset[1], viewport_info.xy_extent[0], viewport_info.xy_extent[1]},
            {viewport_info.z_min, viewport_info.z_max},
            {scissor.offset[0], scissor.offset[1], scissor.extent[0], scissor.extent[1]});
        if (!rectangle || (convert_z_to_float24 && rectangle->depth != 0)) return false;
        rectangles[i] = *rectangle;
        for (float& component : colors[i]) {
          component *= system_constants_.color_exp_bias[0];
          if (!std::isfinite(component)) return false;
        }
      }
      deferred_command_list_.BeginDebugMarker("PinyonShift native FH1 rectangle clear");
      const bool cleared = render_target_cache_->ClearFh1Rectangles(
          std::span<const Fh1ClearRectangle>(rectangles).first(count),
          std::span<const std::array<float, 4>>(colors).first(count), normalized_color_mask != 0,
          normalized_depth_control.z_enable && normalized_depth_control.z_write_enable,
          normalized_depth_control.stencil_enable, uint8_t(ff_stencil_ref_));
      deferred_command_list_.EndDebugMarker();
      return cleared;
    };
    if (try_clear()) return finish_draw(true);
  }

  if (fh1_tonemap_source) {
    deferred_command_list_.BeginDebugMarker(
        "PinyonShift V5 native FH1 tone map");
    const bool native_tonemap_drawn = DrawFh1ToneMap(
        *fh1_tonemap_source, fh1_tonemap_target_format,
        system_constants_.color_exp_bias[0]);
    deferred_command_list_.EndDebugMarker();
    if (native_tonemap_drawn) {
      ++fh1_tonemap_native_draws_;
      if (fh1_tonemap_native_draws_ == 1) {
        REXGPU_INFO("FH1 V5 native tone-map draws 1");
      }
      return finish_draw(true);
    }
  }

  if (!pipeline_handle) {
    if (!configure_guest_pipeline()) {
      return finish_draw(false);
    }
    if (REXCVAR_GET(async_shader_compilation) &&
        pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) == nullptr) {
      return finish_draw(true);
    }
  }
  bind_prepared_guest_pipeline();

  D3D12_GPU_VIRTUAL_ADDRESS depth_index_address = 0;
  // Blended/lit use fixed materials; shadow/packed world retain pixel tables.
  const auto fh1_mesh_root = fh1_scene_stride == 32 ? root_signature_fh1_skinned_ :
      fh1_scene_stride == 16 || fh1_scene_stride == 20
      ? root_signature_fh1_layered_ : root_signature_fh1_depth_;
  if (fh1_depth_indices && (fh1_depth_stride ||
          (fh1_scene_stride && fh1_mesh_root && root_signature == fh1_mesh_root) ||
          (root_signature_fh1_terrain_ && root_signature == root_signature_fh1_terrain_)) &&
      primitive_processing_result.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
    const uint32_t index_width = primitive_processing_result.host_index_format ==
        xenos::IndexFormat::kInt16 ? 2 : 4;
    const uint64_t index_bytes = uint64_t(primitive_processing_result.host_draw_vertex_count) * index_width;
    if (index_bytes && index_bytes <= SharedMemory::kBufferSize) {
      depth_index_address = GetFh1OwnedGeometry(
          primitive_processing_result.guest_index_base, uint32_t(index_bytes),
          ((fh1_mesh_root && root_signature == fh1_mesh_root) ||
           (root_signature_fh1_terrain_ && root_signature == root_signature_fh1_terrain_)) &&
              index_bytes <= (1u << 20));
      if (depth_index_address) {
        // Geometry views retain low address bits in fetch constants; indices do not.
        depth_index_address += primitive_processing_result.guest_index_base & 15;
      }
    }
  }

  D3D12_GPU_VIRTUAL_ADDRESS geometry_address = 0;
  if (fh1_vertex_hash == 0x3BC346726C1C2535ull &&
      root_signature_fh1_layered_ && root_signature == root_signature_fh1_layered_ &&
      primitive_processing_result.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    uint32_t system_words[8];
    std::memcpy(system_words, &system_constants_, sizeof(system_words));
    float scale;
    std::memcpy(&scale, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 254 * 4], sizeof(scale));
    const auto range = geometry_range(
        std::span<const uint32_t, 8>(system_words), scale,
        primitive_processing_result.host_draw_vertex_count,
        regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190],
        regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 191]);
    if (range) {
      geometry_address = GetFh1OwnedGeometry(range->first, range->second);
    }
  }

  // Bounds must describe the cached indices actually bound below. A vertex
  // import can refresh an aliased cache window, so prove them again afterward.
  if (depth_index_address && fh1_mesh_root && root_signature == fh1_mesh_root) {
    const uint32_t width = primitive_processing_result.host_index_format ==
        xenos::IndexFormat::kInt16 ? 2 : 4;
    const uint64_t bytes = uint64_t(primitive_processing_result.host_draw_vertex_count) * width;
    if (bytes && bytes <= (1u << 20)) {
      uint32_t system_words[8];
      std::memcpy(system_words, &system_constants_, sizeof(system_words));
      const auto snapshot_range = [&]() -> std::optional<std::pair<uint32_t, uint32_t>> {
        return GetFh1DepthGeometryRange(
            primitive_processing_result.guest_index_base, uint32_t(bytes),
            std::span<const uint32_t, 8>(system_words), width, fh1_scene_stride ? fh1_scene_stride : fh1_depth_stride,
            regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190],
            regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 191],
            primitive_processing_result.host_primitive_reset_enabled,
            fh1_scene_stride ? fh1_scene_stride : 12);
      };
      if (const auto range = snapshot_range()) {
        const uint64_t imports_before = fh1_geometry_imports_;
        geometry_address = GetFh1OwnedGeometry(range->first, range->second);
        if (geometry_address && fh1_geometry_imports_ != imports_before) {
          const auto current = snapshot_range();
          if (!current || current->first != range->first || current->second > range->second)
            geometry_address = 0;
        }
      }
    }
  }

  if (fh1_scene_stride && !geometry_address) depth_index_address = 0;

  std::array<D3D12_GPU_VIRTUAL_ADDRESS, 2> terrain_addresses{};
  const bool fh1_skinned = root_signature_fh1_skinned_ && root_signature == root_signature_fh1_skinned_;
  if (fh1_skinned && geometry_address && depth_index_address) {
    float offset;
    std::memcpy(&offset, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 156 * 4], sizeof(offset));
    const auto palette = skinned_transform_range(offset,
        regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 188],
        regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 189]);
    const uint32_t width = primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 2 : 4;
    const uint32_t bytes = primitive_processing_result.host_draw_vertex_count * width;
    uint32_t system[8];
    std::memcpy(system, &system_constants_, sizeof(system));
    const auto primary_range = [&]() {
      return GetFh1DepthGeometryRange(primitive_processing_result.guest_index_base, bytes,
          system, width, 32, regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190],
          regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 191],
          primitive_processing_result.host_primitive_reset_enabled, 32);
    };
    const auto before = primary_range();
    if (palette && before) {
      const uint64_t imports_before = fh1_geometry_imports_;
      terrain_addresses[0] = GetFh1OwnedGeometry(palette->first, palette->second);
      // An aliased import can refresh the immutable index snapshot.
      if (terrain_addresses[0] && fh1_geometry_imports_ != imports_before &&
          primary_range() != before) terrain_addresses[0] = 0;
    }
    if (!terrain_addresses[0]) geometry_address = depth_index_address = 0;
  }

  if (fh1_terrain_depth && depth_index_address && root_signature_fh1_terrain_ &&
      root_signature == root_signature_fh1_terrain_) {
    const auto& map = vertex_shader->constant_register_map();
    const uint32_t width = primitive_processing_result.host_index_format ==
        xenos::IndexFormat::kInt16 ? 2 : 4;
    const uint64_t bytes = uint64_t(primitive_processing_result.host_draw_vertex_count) * width;
    if (bytes && bytes <= (1u << 20) && !map.float_dynamic_addressing &&
        map.float_count >= 10 && map.float_count <= 11) {
      // Match UpdateBindings' ascending register-map packing exactly.
      float constants[44]{};
      uint32_t count = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = map.float_bitmap[i];
        uint32_t index;
        while (rex::bit_scan_forward(bits, &index)) {
          bits &= ~(uint64_t(1) << index);
          if (count < 11) std::memcpy(constants + count * 4,
              &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (index << 2)], 4 * sizeof(float));
          ++count;
        }
      }
      if (count == map.float_count) {
        uint32_t system_words[8], fetches[6];
        std::memcpy(system_words, &system_constants_, sizeof(system_words));
        const uint32_t streams[] = {95, 90, 89};
        for (uint32_t i = 0; i < 3; ++i) std::memcpy(fetches + i * 2,
            &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + streams[i] * 2], 2 * sizeof(uint32_t));
        const auto snapshot_ranges = [&]() {
          return GetFh1TerrainGeometryRanges(primitive_processing_result.guest_index_base,
              uint32_t(bytes), system_words, width,
              primitive_processing_result.host_primitive_reset_enabled, constants, fetches);
        };
        if (const auto ranges = snapshot_ranges()) {
          const uint64_t imports_before = fh1_geometry_imports_;
          std::array<D3D12_GPU_VIRTUAL_ADDRESS, 3> addresses{};
          bool complete = true;
          for (uint32_t i = 0; i < 3; ++i) {
            if (!(*ranges)[i].second) continue;
            addresses[i] = GetFh1OwnedGeometry((*ranges)[i].first, (*ranges)[i].second);
            if (!addresses[i]) { complete = false; break; }
          }
          if (complete && fh1_geometry_imports_ != imports_before) {
            const auto current = snapshot_ranges();
            complete = bool(current);
            if (current) for (uint32_t i = 0; i < 3; ++i) {
              if ((*current)[i].first != (*ranges)[i].first ||
                  (*current)[i].second > (*ranges)[i].second) complete = false;
            }
          }
          if (complete) {
            geometry_address = addresses[0];
            terrain_addresses = {addresses[1], addresses[2]};
          }
        }
      }
    }
    // Partial terrain ownership returns every input to shared memory.
    if (!geometry_address) depth_index_address = 0;
  }

  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "This is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return finish_draw(false);
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return finish_draw(false);
      }
      // The native layered root reads fetch 95 from the owned buffer.
      if (geometry_address && (vfetch_index == 95 ||
          (terrain_addresses[1] && (vfetch_index == 89 || vfetch_index == 90)) ||
          (fh1_skinned && terrain_addresses[0] && vfetch_index == 94))) continue;
      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      if (state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        continue;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR(
            "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
            "shared memory",
            vfetch_constant.address << 2, vfetch_constant.size << 2);
        return finish_draw(false);
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }

  // Update constant buffers, descriptors and root parameters.
  if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used, geometry_address, terrain_addresses)) {
    return finish_draw(false);
  }
  // Must not call anything that can change the descriptor heap from now on!

  // Gather memexport ranges and ensure the heaps for them are resident, and
  // also load the data surrounding the export and to fill the regions that
  // won't be modified by the shaders.
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    if (!shared_memory_->RequestRange(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes)) {
      REXGPU_ERROR(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      return finish_draw(false);
    }
  }
  if (memexport_used && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved "
          "memexport destinations");
      return finish_draw(false);
    }
  }

  // Primitive topology.
  D3D_PRIMITIVE_TOPOLOGY primitive_topology;
  if (primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      // TODO(Triang3l): Support all primitive types.
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      default:
        REXGPU_ERROR(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return finish_draw(false);
    }
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        REXGPU_ERROR(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return finish_draw(false);
    }
  }
  SetPrimitiveTopology(primitive_topology);
  // Must not call anything that may change the primitive topology from now on!

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else if (!geometry_address) {
      // The owned layered draw has no shared vertex, pixel or index reads.
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    bind_prepared_guest_pipeline();
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    deferred_command_list_.D3DDrawInstanced(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
  } else {
    D3D12_INDEX_BUFFER_VIEW index_buffer_view;
    index_buffer_view.SizeInBytes = primitive_processing_result.host_draw_vertex_count;
    if (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16) {
      index_buffer_view.SizeInBytes *= sizeof(uint16_t);
      index_buffer_view.Format = DXGI_FORMAT_R16_UINT;
    } else {
      index_buffer_view.SizeInBytes *= sizeof(uint32_t);
      index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
    }
    ID3D12Resource* scratch_index_buffer = nullptr;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
        if (depth_index_address) {
          index_buffer_view.BufferLocation = depth_index_address;
          break;
        }
        if (fh1_depth_indices && !shared_memory_->RequestRange(
                primitive_processing_result.guest_index_base, index_buffer_view.SizeInBytes)) {
          return finish_draw(false);
        }
        if (memexport_used) {
          // If the shared memory is a UAV, it can't be used as an index buffer
          // (UAV is a read/write state, index buffer is a read-only state).
          // Need to copy the indices to a buffer in the index buffer state.
          scratch_index_buffer = RequestScratchGPUBuffer(index_buffer_view.SizeInBytes,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
          if (scratch_index_buffer == nullptr) {
            return finish_draw(false);
          }
          shared_memory_->UseAsCopySource();
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              scratch_index_buffer, 0, shared_memory_->GetBuffer(),
              primitive_processing_result.guest_index_base, index_buffer_view.SizeInBytes);
          PushTransitionBarrier(scratch_index_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_INDEX_BUFFER);
          index_buffer_view.BufferLocation = scratch_index_buffer->GetGPUVirtualAddress();
        } else {
          index_buffer_view.BufferLocation =
              shared_memory_->GetGPUAddress() + primitive_processing_result.guest_index_base;
        }
      } break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer_view.BufferLocation = primitive_processor_->GetConvertedIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer_view.BufferLocation = primitive_processor_->GetBuiltinIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return finish_draw(false);
    }
    deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else if (!geometry_address || !depth_index_address) {
      // Fully owned depth draws have no remaining shared-buffer reads.
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    bind_prepared_guest_pipeline();
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    deferred_command_list_.D3DDrawIndexedInstanced(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
    if (scratch_index_buffer != nullptr) {
      ReleaseScratchGPUBuffer(scratch_index_buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
  }


  if (memexport_used) {
    PROFILE_MEMEXPORT_DRAW();
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.
    // TODO(Triang3l): Find some PM4 command that can be used for indication of
    // when memexports should be awaited?
    shared_memory_->MarkUAVWritesCommitNeeded();
    // Invalidate textures in memexported memory and watch for changes.
    if (!memexport_ranges_.empty()) {
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                          memexport_range.size_bytes);
      }
    } else {
      // Stream constants can be invalid or dynamic, so exact destinations may
      // be unknown. Keep invalidation conservative in this case.
      shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
    }
  }

  return finish_draw(true);
}

void D3D12CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  const auto fallback = [&] { CommandProcessor::ExecuteIndirectBuffer(ptr, count); };
  if (!REXCVAR_GET(fh1_native_reflection_mips) || count != Fh1MipChain::kCommandBytes / 4 ||
      !Fh1MipPhysicalRange(ptr, Fh1MipChain::kCommandBytes)) {
    fallback();
    return;
  }
  ++fh1_mip_candidates_;
  if (fh1_mip_replacement_active_ || active_occlusion_query_.valid ||
      !Fh1MipInheritedState(register_file_->values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
                            register_file_->values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR],
                            register_file_->values[XE_GPU_REG_VGT_MIN_VTX_INDX],
                            register_file_->values[XE_GPU_REG_VGT_MAX_VTX_INDX],
                            register_file_->values[XE_GPU_REG_SQ_VS_CONST],
                            register_file_->values[XE_GPU_REG_SQ_PS_CONST]) ||
      modern_occlusion_query_active_index_ != UINT32_MAX ||
      modern_occlusion_query_active_report_ != ZPDLifecycle::kInvalidReportHandle ||
      !(bin_select_ & bin_mask_) ||
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth ||
      render_target_cache_->GetPath() != RenderTargetCache::Path::kHostRenderTargets) {
    ++fh1_mip_rejections_[1];
    fallback();
    return;
  }
  // Re-read current allocation contents; no pointer/handle is a cache identity.
  const bool probe = REXCVAR_GET(fh1_mip_decode_probe);
  const auto contract_start = probe ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
  std::array<unsigned char, Fh1MipChain::kCommandBytes> commands;
  Fh1MipChain chain;
  const auto copy = [&](uint32_t address, std::span<uint8_t> bytes) {
    return shared_memory_->CopyCpuSnapshot(address, bytes);
  };
  if (!copy(ptr, commands) || !ParseFh1MipChain(commands, chain) ||
      !(ptr + commands.size() <= chain.base || ptr >= chain.base + Fh1MipChain::kTotalBytes) ||
      !CheckFh1MipInputs(chain, copy)) {
    ++fh1_mip_rejections_[2];
    fallback();
    return;
  }
  const auto native_start = probe ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
  const uint32_t face = chain.face;
  if (!BeginSubmission(true) || !texture_cache_->GenerateFh1ReflectionMips(chain.base, face)) {
    ++fh1_mip_rejections_[3];
    fallback();
    return;
  }
  const auto replay_start = probe ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
  fh1_mip_skipped_draws_ = fh1_mip_skipped_copies_ = 0;
  fh1_mip_replacement_active_ = true;
  memory::RingBuffer reader(commands.data(), commands.size());
  reader.set_write_offset(commands.size());
  bool succeeded = true;
  do {
    if (!ExecutePacket(&reader)) {
      succeeded = false;
      break;
    }
  } while (reader.read_count());
  fh1_mip_replacement_active_ = false;
  if (!succeeded || fh1_mip_skipped_draws_ != 8 || fh1_mip_skipped_copies_ != 8) {
    REXGPU_ERROR("FH1 native reflection mip contract execution failed");
    assert_always();
  } else {
    ++fh1_mip_native_faces_;
    if (probe) {
      const auto end = std::chrono::steady_clock::now();
      ++fh1_mip_probe_faces_;
      fh1_mip_probe_contract_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          native_start - contract_start).count());
      fh1_mip_probe_native_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          replay_start - native_start).count());
      fh1_mip_probe_replay_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          end - replay_start).count());
    }
  }
}

bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  if (!BeginSubmission(true)) {
    return false;
  }
  uint32_t written_address = 0;
  uint32_t written_length = 0;
  BeginFh1GpuPassTimingCopy();
  const bool copy_succeeded =
      render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                    written_length, fh1_mip_replacement_active_);
  if (copy_succeeded && REXCVAR_GET(fh1_post_chain_probe)) {
    REXGPU_INFO(
        "FH1 resolve publication {{\"frame\":{},\"address\":\"{:08X}\","
        "\"length\":{},\"copy_control\":{},\"dest_info\":{},"
        "\"dest_pitch\":{},\"surface_info\":{}}}",
        observation_frame_sequence_, written_address, written_length,
        register_file_->Get<reg::RB_COPY_CONTROL>().value,
        register_file_->Get<reg::RB_COPY_DEST_INFO>().value,
        register_file_->Get<reg::RB_COPY_DEST_PITCH>().value,
        register_file_->Get<reg::RB_SURFACE_INFO>().value);
  }
  if (fh1_mip_replacement_active_ && copy_succeeded)
    ++fh1_mip_skipped_copies_;

  auto copy_observer = graphics_system_->copy_observer();
  if (!Fh1ObserveCorpusFrame(observation_frame_sequence_)) {
    copy_observer = nullptr;
  }
  if (copy_observer) {
    system::GraphicsCopyObservation observation;
    observation.frame_sequence = observation_frame_sequence_;
    observation.copy_sequence = ++observation_copy_sequence_;
    observation.current_submission = GetCurrentSubmission();
    observation.completed_submission = submission_completed_;
    observation.written_address = written_address;
    observation.written_length = written_length;
    observation.rb_copy_control = register_file_->Get<reg::RB_COPY_CONTROL>().value;
    observation.rb_copy_dest_base = (*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE];
    observation.rb_copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>().value;
    observation.rb_copy_dest_pitch = register_file_->Get<reg::RB_COPY_DEST_PITCH>().value;
    observation.surface_info = register_file_->Get<reg::RB_SURFACE_INFO>().value;
    for (uint32_t i = 0; i < 4; ++i) {
      observation.color_info[i] =
          (*register_file_)[reg::RB_COLOR_INFO::rt_register_indices[i]];
    }
    observation.depth_info = register_file_->Get<reg::RB_DEPTH_INFO>().value;
    render_target_cache_->PopulateCopySourceTopology(observation);
    observation.succeeded = copy_succeeded;
    auto& fh1_key = observation.fh1_execution_key;
    fh1_key.kind = system::GraphicsFh1ExecutionKind::kCopyResolve;
    const auto append = [](uint64_t& hash, uint64_t value) {
      hash = HashFh1ExecutionValue(hash, value);
    };
    fh1_key.attachment_state = 0xCBF29CE484222325ull;
    append(fh1_key.attachment_state, observation.surface_info);
    for (uint32_t color_info : observation.color_info) {
      append(fh1_key.attachment_state, color_info);
    }
    append(fh1_key.attachment_state, observation.depth_info);
    for (uint64_t value :
         {uint64_t(observation.source_target_base_tiles),
          uint64_t(observation.source_target_pitch_tiles_at_32bpp),
          uint64_t(observation.resolve_source_base_tiles),
          uint64_t(observation.resolve_source_pitch_tiles),
          uint64_t(observation.resolve_source_format),
          uint64_t(observation.resolve_source_guest_msaa_samples)}) {
      append(fh1_key.attachment_state, value);
    }
    fh1_key.resource_state = 0xCBF29CE484222325ull;
    for (uint64_t value :
         {uint64_t(observation.rb_copy_dest_base),
          uint64_t(observation.rb_copy_dest_info),
          uint64_t(observation.rb_copy_dest_pitch),
          uint64_t(observation.written_address),
          uint64_t(observation.written_length)}) {
      append(fh1_key.resource_state, value);
    }
    fh1_key.dynamic_state = 0xCBF29CE484222325ull;
    for (uint64_t value :
         {uint64_t(observation.rb_copy_control),
          uint64_t(observation.source_resource_width),
          uint64_t(observation.source_resource_height),
          uint64_t(observation.source_resource_format),
          uint64_t(observation.source_sample_count),
          uint64_t(observation.source_sample_quality),
          uint64_t(observation.source_guest_msaa_samples),
          uint64_t(observation.draw_resolution_scale_x),
          uint64_t(observation.draw_resolution_scale_y)}) {
      append(fh1_key.dynamic_state, value);
    }
    fh1_key.operation_state = 0xCBF29CE484222325ull;
    for (uint64_t value :
         {uint64_t(observation.resolve_guest_offset_x),
          uint64_t(observation.resolve_guest_offset_y),
          uint64_t(observation.resolve_guest_width),
          uint64_t(observation.resolve_guest_height),
          uint64_t(observation.resolve_physical_offset_x),
          uint64_t(observation.resolve_physical_offset_y),
          uint64_t(observation.resolve_physical_width),
          uint64_t(observation.resolve_physical_height),
          uint64_t(observation.resolve_dest_offset_x),
          uint64_t(observation.resolve_dest_offset_y),
          uint64_t(observation.resolve_dest_pitch),
          uint64_t(observation.resolve_dest_height),
          uint64_t(observation.resolve_sample_select)}) {
      append(fh1_key.operation_state, value);
    }
    fh1_key.hazard_flags = uint32_t(!observation.resolve_info_valid) |
                           (uint32_t(!observation.source_target_available)
                            << 1);
    fh1_key.flags = uint32_t(observation.succeeded) |
                    (uint32_t(observation.native_2x_msaa) << 1);
    fh1_key.identity = fh1_key.ComputeIdentity();
    ObserveFh1GpuPassTimingCopy(fh1_key, observation.frame_sequence);
    if (copy_observer) {
      copy_observer(observation);
    }
  }
  return copy_succeeded;
}

void D3D12CommandProcessor::LogFh1TextureReloadConsumer(
    const D3D12TextureCache::TextureKey& key) const {
  if (!REXCVAR_GET(fh1_post_chain_probe)) {
    return;
  }
  const auto* vertex_shader = static_cast<const D3D12Shader*>(active_vertex_shader());
  const auto* pixel_shader = static_cast<const D3D12Shader*>(active_pixel_shader());
  REXGPU_INFO(
      "FH1 texture reload consumer {{\"frame\":{},\"base\":\"{:08X}\","
      "\"width\":{},\"height\":{},\"format\":{},\"vs\":\"{:016X}\","
      "\"ps\":\"{:016X}\"}}",
      observation_frame_sequence_, uint32_t(key.base_page << 12), key.GetWidth(),
      key.GetHeight(), uint32_t(key.format),
      vertex_shader ? vertex_shader->ucode_data_hash() : 0,
      pixel_shader ? pixel_shader->ucode_data_hash() : 0);
}

bool D3D12CommandProcessor::AwaitFence(ID3D12Fence* fence, uint64_t value,
                                       bool allow_shutdown) {
  if (!allow_shutdown && IsShutdownRequested()) {
    return false;
  }
  const HRESULT event_result = fence->SetEventOnCompletion(value, fence_completion_event_);
  if (FAILED(event_result)) {
    REXGPU_ERROR("Failed to register Direct3D 12 fence completion: 0x{:08X}",
                 static_cast<unsigned>(event_result));
    return false;
  }
  PROFILE_CMD_BUFFER_STALL();
  for (;;) {
    const uint64_t completed = fence->GetCompletedValue();
    const HRESULT removal_reason = GetD3D12Provider().GetDevice()->GetDeviceRemovedReason();
    if (completed == UINT64_MAX || FAILED(removal_reason)) {
      device_removed_ = true;
      LogDeviceRemovalDiagnostics(GetD3D12Provider().GetDevice(),
                                  FAILED(removal_reason) ? removal_reason : DXGI_ERROR_DEVICE_REMOVED);
      return false;
    }
    if (!allow_shutdown && IsShutdownRequested()) {
      return false;
    }
    if (completed >= value) {
      return true;
    }
    // Events can be stale after an interrupted wait. Only the fence proves completion.
    const DWORD wait_result = WaitForSingleObject(fence_completion_event_, 100);
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT) {
      const DWORD error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_INVALID_FUNCTION;
      REXGPU_ERROR("Failed to wait for Direct3D 12 fence: result {}, error {}", wait_result, error);
      return false;
    }
  }
}

bool D3D12CommandProcessor::CheckSubmissionFence(uint64_t await_submission,
                                                 bool allow_shutdown) {
  if (device_removed_ || (!allow_shutdown && IsShutdownRequested())) {
    return false;
  }
  if (await_submission >= submission_current_) {
    if (submission_open_ && !EndSubmission(false)) {
      return false;
    }
    // Include operations such as UpdateTileMappings made outside a submission.
    if (queue_operations_done_since_submission_signal_) {
      const UINT64 fence_value = ++queue_operations_since_submission_fence_last_;
      const HRESULT signal_result = GetD3D12Provider().GetDirectQueue()->Signal(
          queue_operations_since_submission_fence_, fence_value);
      if (FAILED(signal_result)) {
        REXGPU_ERROR("Failed to signal out-of-submission Direct3D 12 fence: 0x{:08X}",
                     static_cast<unsigned>(signal_result));
        return false;
      }
      if (!AwaitFence(queue_operations_since_submission_fence_, fence_value, allow_shutdown)) {
        return false;
      }
      queue_operations_done_since_submission_signal_ = false;
    }
    await_submission = submission_current_ - 1;
  }

  const uint64_t submission_completed_before = submission_completed_;
  uint64_t completed = submission_fence_->GetCompletedValue();
  if (completed == UINT64_MAX) {
    device_removed_ = true;
    LogDeviceRemovalDiagnostics(GetD3D12Provider().GetDevice(), DXGI_ERROR_DEVICE_REMOVED);
    return false;
  }
  if (completed < await_submission) {
    if (!AwaitFence(submission_fence_, await_submission, allow_shutdown)) {
      return false;
    }
    completed = submission_fence_->GetCompletedValue();
    if (completed == UINT64_MAX || completed < await_submission) {
      return false;
    }
  }
  submission_completed_ = completed;
  RetireModernZPDQueries();
  RetireNativeGuestOutputGpuTimings();
  if (submission_completed_ <= submission_completed_before) {
    // Not updated - no need to reclaim or download things.
    return true;
  }
  perf::TraceCriticalPath("gpu_completion",
                          perf::GetTotalCounter(perf::CounterId::kSourceFrameCount),
                          int64_t(submission_completed_));

  // Reclaim command allocators.
  while (command_allocator_submitted_first_) {
    if (command_allocator_submitted_first_->last_usage_submission > submission_completed_) {
      break;
    }
    if (command_allocator_writable_last_) {
      command_allocator_writable_last_->next = command_allocator_submitted_first_;
    } else {
      command_allocator_writable_first_ = command_allocator_submitted_first_;
    }
    command_allocator_writable_last_ = command_allocator_submitted_first_;
    command_allocator_submitted_first_ = command_allocator_submitted_first_->next;
    command_allocator_writable_last_->next = nullptr;
  }
  if (!command_allocator_submitted_first_) {
    command_allocator_submitted_last_ = nullptr;
  }

  // Release single-use bindless descriptors.
  while (!view_bindless_one_use_descriptors_.empty()) {
    if (view_bindless_one_use_descriptors_.front().second > submission_completed_) {
      break;
    }
    ReleaseViewBindlessDescriptorImmediately(view_bindless_one_use_descriptors_.front().first);
    view_bindless_one_use_descriptors_.pop_front();
  }

  // Delete transient resources marked for deletion.
  while (!resources_for_deletion_.empty()) {
    if (resources_for_deletion_.front().first > submission_completed_) {
      break;
    }
    resources_for_deletion_.front().second->Release();
    resources_for_deletion_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);
  return true;
}

void D3D12CommandProcessor::LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason) {
  const char* reason_str = "Unknown";
  switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG:
      reason_str = "DEVICE_HUNG (TDR - GPU command took too long)";
      break;
    case DXGI_ERROR_DEVICE_REMOVED:
      reason_str = "DEVICE_REMOVED (driver internal error or hot-unplug)";
      break;
    case DXGI_ERROR_DEVICE_RESET:
      reason_str = "DEVICE_RESET (bad GPU command)";
      break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      reason_str = "DRIVER_INTERNAL_ERROR";
      break;
    case DXGI_ERROR_INVALID_CALL:
      reason_str = "INVALID_CALL";
      break;
  }
  REXGPU_ERROR("D3D12 device removed: HRESULT 0x{:08X} - {}", static_cast<unsigned>(reason),
               reason_str);

  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node;
         node = node->pNext) {
      if (!node->pLastBreadcrumbValue || !node->pCommandHistory ||
          *node->pLastBreadcrumbValue == 0) {
        continue;
      }
      REXGPU_ERROR("DRED breadcrumb: completed {} of {} ops", *node->pLastBreadcrumbValue,
                   node->BreadcrumbCount);
      uint32_t last = std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount);
      uint32_t start = last > 3 ? last - 3 : 0;
      uint32_t end = std::min(last + 1, node->BreadcrumbCount);
      for (uint32_t i = start; i < end; i++) {
        REXGPU_ERROR("  [{}] op type {}{}", i, static_cast<int>(node->pCommandHistory[i]),
                     i == last ? " <-- FAULT" : "");
      }
    }
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) && page_fault.PageFaultVA != 0) {
    REXGPU_ERROR("DRED page fault at VA 0x{:016X}", page_fault.PageFaultVA);
  }
}

bool D3D12CommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_removed_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check if the device is still available.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  HRESULT device_removed_reason = device->GetDeviceRemovedReason();
  if (FAILED(device_removed_reason)) {
    device_removed_ = true;
    LogDeviceRemovalDiagnostics(device, device_removed_reason);
    // The GPU-loss callback terminates the process. Persist the HRESULT and
    // any available DRED breadcrumbs before control reaches the fatal path.
    rex::FlushLogging();
    if (graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(device_removed_reason !=
                                                   DXGI_ERROR_DEVICE_REMOVED);
    }
    return false;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame.
  if (!CheckSubmissionFence(is_opening_frame
                                ? closed_frame_submissions_[frame_current_ % kQueueFrames]
                                : 0)) {
    return false;
  }
  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command list - will submit it to the real one in the
    // end of the submission (when async pipeline creation requests are
    // fulfilled).
    deferred_command_list_.Reset();
    if (is_opening_frame) {
      BeginNativeGuestOutputGpuTimingFrame();
    }

    // Reset cached state of the command list.
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
    viewport_cache_valid_ = false;
    current_guest_pipeline_ = nullptr;
    current_external_pipeline_ = nullptr;
    current_graphics_root_signature_ = nullptr;
    current_graphics_root_up_to_date_ = 0;
    if (bindless_resources_used_) {
      deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                sampler_bindless_heap_current_);
    } else {
      view_bindful_heap_current_ = nullptr;
      sampler_bindful_heap_current_ = nullptr;
    }
    primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    render_target_cache_->BeginSubmission();

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(submission_current_);
  }

  if (is_opening_frame) {
    frame_open_ = true;
    // Reset bindings that depend on the data stored in the pools.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    current_shared_memory_binding_is_uav_.reset();
    if (bindless_resources_used_) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else {
      draw_view_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      draw_sampler_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    constant_buffer_pool_->Reclaim(frame_completed_);
    if (!bindless_resources_used_) {
      view_bindful_heap_pool_->Reclaim(frame_completed_);
      sampler_bindful_heap_pool_->Reclaim(frame_completed_);
    }
    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  if ((GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kStrict) &&
      zpd_lifecycle_.active().segment_pending_begin) {
    OpenModernZPDSegment();
  }

  return true;
}

bool D3D12CommandProcessor::EndSubmission(bool is_swap) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();

  // Make sure there is a command allocator to write commands to.
  if (submission_open_ && !command_allocator_writable_first_) {
    ID3D12CommandAllocator* command_allocator;
    if (FAILED(provider.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&command_allocator)))) {
      REXGPU_ERROR("Failed to create a command allocator");
      // Try to submit later. Completely dropping the submission is not
      // permitted because resources would be left in an undefined state.
      return false;
    }
    command_allocator_writable_first_ = new CommandAllocator;
    command_allocator_writable_first_->command_allocator = command_allocator;
    command_allocator_writable_first_->last_usage_submission = 0;
    command_allocator_writable_first_->next = nullptr;
    command_allocator_writable_last_ = command_allocator_writable_first_;
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    EndFh1GpuPassTimingFrame();

    texture_cache_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    if ((GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kStrict) &&
        modern_occlusion_query_active_index_ != UINT32_MAX) {
      CloseModernZPDSegment();
    } else if (active_occlusion_query_.valid && occlusion_query_heap_) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         active_occlusion_query_.host_index);
      active_occlusion_query_ = {};
    }

    pipeline_cache_->EndSubmission();

    // Submit barriers now because resources with the queued barriers may be
    // destroyed between frames.
    SubmitBarriers();

    if (is_closing_frame) {
      EndNativeGuestOutputGpuTimingFrame();
    }

    ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

    // Submit the deferred command list.
    // Only one deferred command list must be executed in the same
    // ExecuteCommandLists - the boundaries of ExecuteCommandLists are a full
    // UAV and aliasing barrier, and subsystems of the emulator assume it
    // happens between Xenia submissions.
    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    command_allocator->Reset();
    command_list_->Reset(command_allocator, nullptr);
    const uint64_t traced_submission = submission_current_;
    perf::TraceCriticalPath("submission_begin",
                            perf::GetTotalCounter(perf::CounterId::kSourceFrameCount),
                            int64_t(traced_submission), is_closing_frame ? 1 : 0);
    deferred_command_list_.Execute(command_list_, command_list_1_);
    command_list_->Close();
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
    command_allocator_writable_first_->last_usage_submission = submission_current_;
    if (command_allocator_submitted_last_) {
      command_allocator_submitted_last_->next = command_allocator_writable_first_;
    } else {
      command_allocator_submitted_first_ = command_allocator_writable_first_;
    }
    command_allocator_submitted_last_ = command_allocator_writable_first_;
    command_allocator_writable_first_ = command_allocator_writable_first_->next;
    command_allocator_submitted_last_->next = nullptr;
    if (!command_allocator_writable_first_) {
      command_allocator_writable_last_ = nullptr;
    }

    direct_queue->Signal(submission_fence_, submission_current_++);
    perf::TraceCriticalPath("submission_end",
                            perf::GetTotalCounter(perf::CounterId::kSourceFrameCount),
                            int64_t(traced_submission), is_closing_frame ? 1 : 0);

    submission_open_ = false;

    // Queue operations done directly (like UpdateTileMappings) will be awaited
    // alongside the last submission if needed.
    queue_operations_done_since_submission_signal_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kQueueFrames] = submission_current_ - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      ClearCommandAllocatorCache();
      ClearFh1OwnedGeometry();

      ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
      scratch_buffer_size_ = 0;

      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
          sampler_bindless_heap_overflowed.first->Release();
        }
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
        view_bindful_heap_pool_->ClearCache();
      }
      constant_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      // Not clearing the root signatures as they're referenced by pipelines,
      // which are not destroyed.

      primitive_processor_->ClearCache();

      render_target_cache_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

bool D3D12CommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_->IsCreatingPipelines();
}

void D3D12CommandProcessor::ClearCommandAllocatorCache() {
  while (command_allocator_submitted_first_) {
    auto next = command_allocator_submitted_first_->next;
    command_allocator_submitted_first_->command_allocator->Release();
    delete command_allocator_submitted_first_;
    command_allocator_submitted_first_ = next;
  }
  command_allocator_submitted_last_ = nullptr;
  while (command_allocator_writable_first_) {
    auto next = command_allocator_writable_first_->next;
    command_allocator_writable_first_->command_allocator->Release();
    delete command_allocator_writable_first_;
    command_allocator_writable_first_ = next;
  }
  command_allocator_writable_last_ = nullptr;
}

void D3D12CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info, const draw_util::Scissor& scissor,
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Viewport.
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  D3D12_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  SetScissorRect(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    const RegisterFile& regs = *register_file_;

    // Blend factor.
    float blend_factor[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    ff_blend_factor_update_needed_ |=
        std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
    if (ff_blend_factor_update_needed_) {
      std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);
      deferred_command_list_.D3DOMSetBlendFactor(ff_blend_factor_);
      ff_blend_factor_update_needed_ = false;
    }

    // Stencil reference value. Per-face reference not supported by Direct3D 12,
    // choose the back face one only if drawing only back faces.
    Register stencil_ref_mask_reg;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
    }
    uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
    ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
    if (ff_stencil_ref_update_needed_) {
      ff_stencil_ref_ = stencil_ref;
      deferred_command_list_.D3DOMSetStencilRef(ff_stencil_ref_);
      ff_stencil_ref_update_needed_ = false;
    }
  }
}

void D3D12CommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  bool edram_rov_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for ROV,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[4];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_rov_used) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_rov_used && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Whether shared memory is an SRV or a UAV. Because a resource can't be in a
  // read-write (UAV) and a read-only (SRV, IBV) state at once, if any shader in
  // the pipeline uses memexport, the shared memory buffer must be a UAV.
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal and SV_IsFrontFace matters.
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_rov_used && depth_stencil_enabled) {
    flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= DxbcShaderTranslator::kSysFlag_ROVDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Tessellation factor range, plus 1.0 according to the images in
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index (or 0 when drawing other primitives or using an
  // index buffer).
  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_index_offset != vgt_indx_offset;
  system_constants_.vertex_index_offset = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Conversion to Direct3D 12 normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_normalized_fixed_point = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    textures_normalized_fixed_point |=
        uint32_t(texture_cache_->IsActiveTextureNormalizedFixedPoint(texture_index))
        << texture_index;
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;
  dirty |= system_constants_.textures_normalized_fixed_point !=
           textures_normalized_fixed_point;
  system_constants_.textures_normalized_fixed_point =
      textures_normalized_fixed_point;

  // Log2 of sample count, for alpha to mask and with ROV, for EDRAM address
  // calculation with MSAA.
  uint32_t sample_count_log2_x = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test and alpha to coverage.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for ROV writing.
  if (edram_rov_used) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and ROV render target writing.
  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
          !render_target_cache_->IsFixed16TruncatedToMinus1To1()) {
        // Remap from -32...32 to -1...1 by dividing the output values by 32,
        // losing blending correctness, but getting the full range.
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale =
        rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (color_exp_bias << 23)));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_rov_used) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
      }
    }
  }

  if (edram_rov_used) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     std::max(draw_resolution_scale_x, draw_resolution_scale_y);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      dirty |= system_constants_.edram_stencil_front_reference != rb_stencilrefmask.stencilref;
      system_constants_.edram_stencil_front_reference = rb_stencilrefmask.stencilref;
      dirty |= system_constants_.edram_stencil_front_read_mask != rb_stencilrefmask.stencilmask;
      system_constants_.edram_stencil_front_read_mask = rb_stencilrefmask.stencilmask;
      dirty |=
          system_constants_.edram_stencil_front_write_mask != rb_stencilrefmask.stencilwritemask;
      system_constants_.edram_stencil_front_write_mask = rb_stencilrefmask.stencilwritemask;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        dirty |= system_constants_.edram_stencil_back_reference != rb_stencilrefmask_bf.stencilref;
        system_constants_.edram_stencil_back_reference = rb_stencilrefmask_bf.stencilref;
        dirty |= system_constants_.edram_stencil_back_read_mask != rb_stencilrefmask_bf.stencilmask;
        system_constants_.edram_stencil_back_read_mask = rb_stencilrefmask_bf.stencilmask;
        dirty |= system_constants_.edram_stencil_back_write_mask !=
                 rb_stencilrefmask_bf.stencilwritemask;
        system_constants_.edram_stencil_back_write_mask = rb_stencilrefmask_bf.stencilwritemask;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 4 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    4 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  cbuffer_binding_system_.up_to_date &= !dirty;
}

bool D3D12CommandProcessor::UpdateBindings(const D3D12Shader* vertex_shader,
                                           const D3D12Shader* pixel_shader,
                                           ID3D12RootSignature* root_signature,
                                           bool shared_memory_is_uav,
                                           D3D12_GPU_VIRTUAL_ADDRESS geometry_address,
                                           const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 2>& terrain_addresses) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  const RegisterFile& regs = *register_file_;

#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const bool native_layered = root_signature_fh1_layered_ &&
                              root_signature == root_signature_fh1_layered_;

  const bool native_terrain = root_signature_fh1_terrain_ &&
                              root_signature == root_signature_fh1_terrain_;

  const bool native_skinned = root_signature_fh1_skinned_ && root_signature == root_signature_fh1_skinned_;
  uint32_t skinned_origin = 0;
  if (native_skinned && terrain_addresses[0]) {
    float offset;
    std::memcpy(&offset, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 156 * 4], sizeof(offset));
    const auto range = skinned_transform_range(offset,
        regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 188],
        regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 189]);
    if (!range) return false;
    skinned_origin = range->first & ~15u;
  }
  if (current_fh1_skinned_origin_ != skinned_origin) {
    current_fh1_skinned_origin_ = skinned_origin;
    cbuffer_binding_fetch_.up_to_date = false;
  }

  // Set the new root signature.
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    if (!bindless_resources_used_) {
      GetRootBindfulExtraParameterIndices(vertex_shader, pixel_shader,
                                          current_graphics_root_bindful_extras_);
    }
    // Changing the root signature invalidates all bindings.
    current_graphics_root_up_to_date_ = 0;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }

  // Select the root parameter indices depending on the used binding model.
  uint32_t root_parameter_fetch_constants = bindless_resources_used_
                                                ? kRootParameter_Bindless_FetchConstants
                                                : kRootParameter_Bindful_FetchConstants;
  uint32_t root_parameter_float_constants_vertex =
      bindless_resources_used_ ? kRootParameter_Bindless_FloatConstantsVertex
                               : kRootParameter_Bindful_FloatConstantsVertex;
  uint32_t root_parameter_float_constants_pixel = bindless_resources_used_
                                                      ? kRootParameter_Bindless_FloatConstantsPixel
                                                      : kRootParameter_Bindful_FloatConstantsPixel;
  uint32_t root_parameter_system_constants = bindless_resources_used_
                                                 ? kRootParameter_Bindless_SystemConstants
                                                 : kRootParameter_Bindful_SystemConstants;
  uint32_t root_parameter_bool_loop_constants = bindless_resources_used_
                                                    ? kRootParameter_Bindless_BoolLoopConstants
                                                    : kRootParameter_Bindful_BoolLoopConstants;
  uint32_t root_parameter_shared_memory_and_bindful_edram =
      bindless_resources_used_ ? kRootParameter_Bindless_SharedMemory
                               : kRootParameter_Bindful_SharedMemoryAndEdram;

  // An owned geometry fetch is rebased to its aligned view. Switching ownership
  // changes its constant bytes; register changes track the remaining low offset.
  if (current_fh1_geometry_address_ != geometry_address) {
    if (bool(current_fh1_geometry_address_) != bool(geometry_address)) {
      cbuffer_binding_fetch_.up_to_date = false;
    }
    current_fh1_geometry_address_ = geometry_address;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_shared_memory_and_bindful_edram);
  }

  if (current_fh1_terrain_addresses_ != terrain_addresses) {
    if (bool(current_fh1_terrain_addresses_[1]) != bool(terrain_addresses[1]) ||
        bool(current_fh1_terrain_addresses_[0]) != bool(terrain_addresses[0])) {
      cbuffer_binding_fetch_.up_to_date = false;
    }
    current_fh1_terrain_addresses_ = terrain_addresses;
    current_graphics_root_up_to_date_ &= ~((1u << kRootParameter_Bindless_DescriptorIndicesVertex) |
                                         (1u << kRootParameter_Bindless_DescriptorIndicesPixel));
  }

  //
  // Update root constant buffers that are common for bindful and bindless.
  //

  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, we can reuse any buffer for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the constant buffer data.
  if (!cbuffer_binding_system_.up_to_date) {
    uint8_t* system_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(system_constants_), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_system_.address);
    if (system_constants == nullptr) {
      return false;
    }
    std::memcpy(system_constants, &system_constants_, sizeof(system_constants_));
    cbuffer_binding_system_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_system_constants);
  }
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    // Even if the shader doesn't need any float constants, a valid binding must
    // still be provided, so if the first draw in the frame with the current
    // root signature doesn't have float constants at all, still allocate an
    // empty buffer.
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_vertex, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_vertex_.address);
    if (float_constants == nullptr) {
      return false;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t float_constant_map_entry = float_constant_map_vertex.float_bitmap[i];
      uint32_t float_constant_index;
      while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
        float_constant_map_entry &= ~(1ull << float_constant_index);
        std::memcpy(
            float_constants,
            &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (float_constant_index << 2)],
            4 * sizeof(float));
        float_constants += 4 * sizeof(float);
      }
    }
    cbuffer_binding_float_vertex_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_vertex);
  }
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_pixel, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_pixel_.address);
    if (float_constants == nullptr) {
      return false;
    }
    if (pixel_shader != nullptr) {
      const Shader::ConstantRegisterMap& float_constant_map_pixel =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = float_constant_map_pixel.float_bitmap[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              float_constants,
              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (float_constant_index << 2)],
              4 * sizeof(float));
          float_constants += 4 * sizeof(float);
        }
      }
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_pixel);
  }
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    uint8_t* bool_loop_constants = constant_buffer_pool_->Request(
        frame_current_, kBoolLoopConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_bool_loop_.address);
    if (bool_loop_constants == nullptr) {
      return false;
    }
    std::memcpy(bool_loop_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    cbuffer_binding_bool_loop_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_bool_loop_constants);
  }
  if (!cbuffer_binding_fetch_.up_to_date) {
    constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
    uint8_t* fetch_constants = constant_buffer_pool_->Request(
        frame_current_, kFetchConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_fetch_.address);
    if (fetch_constants == nullptr) {
      return false;
    }
    std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
    if (geometry_address) {
      const uint32_t rebased = regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190] & 15u;
      std::memcpy(fetch_constants + 190 * sizeof(uint32_t), &rebased, sizeof(rebased));
    }
    for (uint32_t i = 0; i < 2; ++i) {
      if (!terrain_addresses[i]) continue;
      const uint32_t fetch = native_skinned ? 94 : (i == 0 ? 90 : 89);
      const uint32_t original = regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch * 2];
      const uint32_t rebased = native_skinned ? original - skinned_origin : original & 15u;
      std::memcpy(fetch_constants + fetch * 2 * sizeof(uint32_t), &rebased, sizeof(rebased));
    }
    cbuffer_binding_fetch_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_fetch_constants);
  }

  //
  // Update descriptors.
  //

  if (!current_shared_memory_binding_is_uav_.has_value() ||
      current_shared_memory_binding_is_uav_.value() != shared_memory_is_uav) {
    current_shared_memory_binding_is_uav_ = shared_memory_is_uav;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_shared_memory_and_bindful_edram);
  }

  // Get textures and samplers used by the vertex shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_vertex = vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex = vertex_shader->GetSamplerBindingLayoutUserUID();
  const std::vector<D3D12Shader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  const std::vector<D3D12Shader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  size_t texture_count_vertex = textures_vertex.size();
  size_t sampler_count_vertex = samplers_vertex.size();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      bindful_samplers_written_vertex_ = false;
    }
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      D3D12TextureCache::SamplerParameters parameters =
          texture_cache_->GetSamplerParameters(samplers_vertex[i]);
      if (current_samplers_vertex_[i] != parameters) {
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
        bindful_samplers_written_vertex_ = false;
        current_samplers_vertex_[i] = parameters;
      }
    }
  }

  // Get textures and samplers used by the pixel shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_pixel, sampler_layout_uid_pixel;
  const std::vector<D3D12Shader::TextureBinding>* textures_pixel;
  const std::vector<D3D12Shader::SamplerBinding>* samplers_pixel;
  size_t texture_count_pixel, sampler_count_pixel;
  if (pixel_shader != nullptr) {
    texture_layout_uid_pixel = pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel = pixel_shader->GetSamplerBindingLayoutUserUID();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    texture_count_pixel = textures_pixel->size();
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    sampler_count_pixel = samplers_pixel->size();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        bindful_samplers_written_pixel_ = false;
      }
      current_samplers_pixel_.resize(
          std::max(current_samplers_pixel_.size(), size_t(sampler_count_pixel)));
      for (uint32_t i = 0; i < sampler_count_pixel; ++i) {
        D3D12TextureCache::SamplerParameters parameters =
            texture_cache_->GetSamplerParameters((*samplers_pixel)[i]);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          bindful_samplers_written_pixel_ = false;
        }
      }
    }
  } else {
    texture_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    sampler_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    textures_pixel = nullptr;
    texture_count_pixel = 0;
    samplers_pixel = nullptr;
    sampler_count_pixel = 0;
  }

  assert_true(sampler_count_vertex + sampler_count_pixel <= kSamplerHeapSize);

  if (bindless_resources_used_) {
    //
    // Bindless descriptors path.
    //

    // Native fixed bindings may have current layouts without an index upload.
    if (!native_layered) {
      if (!cbuffer_binding_descriptor_indices_vertex_.address)
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      if (!cbuffer_binding_descriptor_indices_pixel_.address)
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }

    // Check if need to write new descriptor indices.
    // Samplers have already been checked.
    if (texture_count_vertex && cbuffer_binding_descriptor_indices_vertex_.up_to_date &&
        (current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_vertex_.data(),
                                                          textures_vertex.data(),
                                                          texture_count_vertex))) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    if (texture_count_pixel && cbuffer_binding_descriptor_indices_pixel_.up_to_date &&
        (current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_pixel_.data(),
                                                          textures_pixel->data(),
                                                          texture_count_pixel))) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }

    // Get sampler descriptor indices, write new samplers, and handle sampler
    // heap overflow if it happens.
    if ((sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) ||
        (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date)) {
      for (uint32_t i = 0; i < 2; ++i) {
        if (i) {
          // Overflow happened - invalidate sampler bindings because their
          // descriptor indices can't be used anymore (and even if heap creation
          // fails, because current_sampler_bindless_indices_#_ are in an
          // undefined state now) and switch to a new sampler heap.
          cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          ID3D12DescriptorHeap* sampler_heap_new;
          if (!sampler_bindless_heaps_overflowed_.empty() &&
              sampler_bindless_heaps_overflowed_.front().second <= submission_completed_) {
            sampler_heap_new = sampler_bindless_heaps_overflowed_.front().first;
            sampler_bindless_heaps_overflowed_.pop_front();
          } else {
            D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_new_desc;
            sampler_heap_new_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            sampler_heap_new_desc.NumDescriptors = kSamplerHeapSize;
            sampler_heap_new_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            sampler_heap_new_desc.NodeMask = 0;
            if (FAILED(device->CreateDescriptorHeap(&sampler_heap_new_desc,
                                                    IID_PPV_ARGS(&sampler_heap_new)))) {
              REXGPU_ERROR(
                  "Failed to create a new bindless sampler descriptor heap "
                  "after an overflow of the previous one");
              return false;
            }
          }
          // Only change the heap if a new heap was created successfully, not to
          // leave the values in an undefined state in case CreateDescriptorHeap
          // has failed.
          sampler_bindless_heaps_overflowed_.push_back(
              std::make_pair(sampler_bindless_heap_current_, submission_current_));
          sampler_bindless_heap_current_ = sampler_heap_new;
          sampler_bindless_heap_cpu_start_ =
              sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_gpu_start_ =
              sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_allocated_ = 0;
          // The only thing the heap is used for now is texture cache samplers -
          // invalidate all of them.
          texture_cache_bindless_sampler_map_.clear();
          deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                    sampler_bindless_heap_current_);
          current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_SamplerHeap);
        }
        bool samplers_overflowed = false;
        if (sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
          current_sampler_bindless_indices_vertex_.resize(std::max(
              current_sampler_bindless_indices_vertex_.size(), size_t(sampler_count_vertex)));
          for (uint32_t j = 0; j < sampler_count_vertex; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_vertex_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_vertex_[j] = sampler_index;
          }
        }
        if (samplers_overflowed) {
          continue;
        }
        if (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
          current_sampler_bindless_indices_pixel_.resize(std::max(
              current_sampler_bindless_indices_pixel_.size(), size_t(sampler_count_pixel)));
          for (uint32_t j = 0; j < sampler_count_pixel; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_pixel_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_pixel_[j] = sampler_index;
          }
        }
        if (!samplers_overflowed) {
          break;
        }
      }
    }

    if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
      uint32_t* descriptor_indices = nullptr;
      if (native_layered) {
        cbuffer_binding_descriptor_indices_vertex_.address = 0;
      } else {
        descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
            frame_current_,
            std::max(texture_count_vertex + sampler_count_vertex, size_t(1)) * sizeof(uint32_t),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
            &cbuffer_binding_descriptor_indices_vertex_.address));
        if (!descriptor_indices) {
          return false;
        }
      }
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        const D3D12Shader::TextureBinding& texture = textures_vertex[i];
        if (descriptor_indices) descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      if (texture_count_vertex) {
        current_texture_srv_keys_vertex_.resize(
            std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                  textures_vertex.data(), texture_count_vertex);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        if (descriptor_indices) descriptor_indices[samplers_vertex[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_vertex_[i];
      }
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesVertex);
    }

    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      uint32_t* descriptor_indices = nullptr;
      if (native_layered) {
        cbuffer_binding_descriptor_indices_pixel_.address = 0;
      } else {
        descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
            frame_current_,
            std::max(texture_count_pixel + sampler_count_pixel, size_t(1)) * sizeof(uint32_t),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
            &cbuffer_binding_descriptor_indices_pixel_.address));
        if (!descriptor_indices) {
          return false;
        }
      }
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        const D3D12Shader::TextureBinding& texture = (*textures_pixel)[i];
        if (descriptor_indices) descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      if (texture_count_pixel) {
        current_texture_srv_keys_pixel_.resize(
            std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                  textures_pixel->data(), texture_count_pixel);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        if (descriptor_indices) descriptor_indices[(*samplers_pixel)[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_pixel_[i];
      }
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = true;
      if (!native_layered) {
        current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesPixel);
      }
    }
  } else {
    //
    // Bindful descriptors path.
    //

    // See what descriptors need to be updated.
    // Samplers have already been checked.
    bool write_textures_vertex =
        texture_count_vertex && (!bindful_textures_written_vertex_ ||
                                 current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
                                 !texture_cache_->AreActiveTextureSRVKeysUpToDate(
                                     current_texture_srv_keys_vertex_.data(),
                                     textures_vertex.data(), texture_count_vertex));
    bool write_textures_pixel =
        texture_count_pixel &&
        (!bindful_textures_written_pixel_ ||
         current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_pixel_.data(), textures_pixel->data(), texture_count_pixel));
    bool write_samplers_vertex = sampler_count_vertex && !bindful_samplers_written_vertex_;
    bool write_samplers_pixel = sampler_count_pixel && !bindful_samplers_written_pixel_;
    bool edram_rov_used =
        render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

    // Allocate the descriptors.
    size_t view_count_partial_update = 0;
    if (write_textures_vertex) {
      view_count_partial_update += texture_count_vertex;
    }
    if (write_textures_pixel) {
      view_count_partial_update += texture_count_pixel;
    }
    // Shared memory SRV and null UAV + null SRV and shared memory UAV +
    // textures.
    size_t view_count_full_update = 4 + texture_count_vertex + texture_count_pixel;
    if (edram_rov_used) {
      // + EDRAM UAV in two tables (with the shared memory SRV and with the
      // shared memory UAV).
      view_count_full_update += 2;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE view_cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE view_gpu_handle;
    uint32_t descriptor_size_view = provider.GetViewDescriptorSize();
    uint64_t view_heap_index = RequestViewBindfulDescriptors(
        draw_view_bindful_heap_index_, uint32_t(view_count_partial_update),
        uint32_t(view_count_full_update), view_cpu_handle, view_gpu_handle);
    if (view_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      REXGPU_ERROR("Failed to allocate view descriptors");
      return false;
    }
    size_t sampler_count_partial_update = 0;
    if (write_samplers_vertex) {
      sampler_count_partial_update += sampler_count_vertex;
    }
    if (write_samplers_pixel) {
      sampler_count_partial_update += sampler_count_pixel;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle = {};
    uint32_t descriptor_size_sampler = provider.GetSamplerDescriptorSize();
    uint64_t sampler_heap_index = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
    if (sampler_count_vertex != 0 || sampler_count_pixel != 0) {
      sampler_heap_index = RequestSamplerBindfulDescriptors(
          draw_sampler_bindful_heap_index_, uint32_t(sampler_count_partial_update),
          uint32_t(sampler_count_vertex + sampler_count_pixel), sampler_cpu_handle,
          sampler_gpu_handle);
      if (sampler_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
        REXGPU_ERROR("Failed to allocate sampler descriptors");
        return false;
      }
    }
    if (draw_view_bindful_heap_index_ != view_heap_index) {
      // Need to update all view descriptors.
      write_textures_vertex = texture_count_vertex != 0;
      write_textures_pixel = texture_count_pixel != 0;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      // If updating fully, write the shared memory SRV and UAV descriptors and,
      // if needed, the EDRAM descriptor.
      // SRV + null UAV + EDRAM.
      gpu_handle_shared_memory_srv_and_edram_ = view_gpu_handle;
      shared_memory_->WriteRawSRVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      ui::d3d12::util::CreateBufferRawUAV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      // Null SRV + UAV + EDRAM.
      gpu_handle_shared_memory_uav_and_edram_ = view_gpu_handle;
      ui::d3d12::util::CreateBufferRawSRV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      shared_memory_->WriteRawUAVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindful_SharedMemoryAndEdram);
    }
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid &&
        draw_sampler_bindful_heap_index_ != sampler_heap_index) {
      write_samplers_vertex = sampler_count_vertex != 0;
      write_samplers_pixel = sampler_count_pixel != 0;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Write the descriptors.
    if (write_textures_vertex) {
      assert_true(current_graphics_root_bindful_extras_.textures_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_vertex_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV(textures_vertex[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      current_texture_srv_keys_vertex_.resize(
          std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                textures_vertex.data(), texture_count_vertex);
      bindful_textures_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_vertex);
    }
    if (write_textures_pixel) {
      assert_true(current_graphics_root_bindful_extras_.textures_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_pixel_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV((*textures_pixel)[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      current_texture_srv_keys_pixel_.resize(
          std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                textures_pixel->data(), texture_count_pixel);
      bindful_textures_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_pixel);
    }
    if (write_samplers_vertex) {
      assert_true(current_graphics_root_bindful_extras_.samplers_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_vertex_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        texture_cache_->WriteSampler(current_samplers_vertex_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_vertex);
    }
    if (write_samplers_pixel) {
      assert_true(current_graphics_root_bindful_extras_.samplers_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_pixel_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        texture_cache_->WriteSampler(current_samplers_pixel_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_pixel);
    }

    // Wrote new descriptors on the current page.
    draw_view_bindful_heap_index_ = view_heap_index;
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      draw_sampler_bindful_heap_index_ = sampler_heap_index;
    }
  }

  // Texture dirtiness need not rebind an unchanged signed view or sampler.
  // Root/heap changes still clear the existing root validity bits.
  if (native_layered) {
    assert_true(texture_count_pixel == 2 && sampler_count_pixel == 1);
    const uint32_t indices[] = {
        texture_cache_->GetActiveTextureBindlessSRVIndex((*textures_pixel)[0]),
        texture_cache_->GetActiveTextureBindlessSRVIndex((*textures_pixel)[1]),
        current_sampler_bindless_indices_pixel_[0]};
    const uint32_t slots[] = {kRootParameter_Bindless_ViewHeap,
        kRootParameter_Bindless_DescriptorIndicesPixel, kRootParameter_Bindless_SamplerHeap};
    for (uint32_t i = 0; i < 3; ++i) {
      if (fh1_fixed_descriptor_indices_[i] != indices[i]) {
        fh1_fixed_descriptor_indices_[i] = indices[i];
        current_graphics_root_up_to_date_ &= ~(1u << slots[i]);
      }
    }
  }

  // Update the root parameters.
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_fetch_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_fetch_constants,
                                                                cbuffer_binding_fetch_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_fetch_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_vertex))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_vertex, cbuffer_binding_float_vertex_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_vertex;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_pixel))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_pixel, cbuffer_binding_float_pixel_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_pixel;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_system_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_system_constants,
                                                                cbuffer_binding_system_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_system_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_bool_loop_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_bool_loop_constants,
                                                                cbuffer_binding_bool_loop_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_bool_loop_constants;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_shared_memory_and_bindful_edram))) {
    if (current_graphics_root_signature_ == root_signature_fh1_layered_ ||
        current_graphics_root_signature_ == root_signature_fh1_depth_ || native_terrain || native_skinned) {
      assert_false(shared_memory_is_uav);
      deferred_command_list_.D3DSetGraphicsRootShaderResourceView(
          root_parameter_shared_memory_and_bindful_edram,
          geometry_address ? geometry_address : shared_memory_->GetGPUAddress());
    } else {
      assert_true(current_shared_memory_binding_is_uav_.has_value());
      D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_and_bindful_edram;
      if (bindless_resources_used_) {
        gpu_handle_shared_memory_and_bindful_edram = provider.OffsetViewDescriptor(
            view_bindless_heap_gpu_start_,
            uint32_t(current_shared_memory_binding_is_uav_.value()
                         ? SystemBindlessView ::kNullRawSRVAndSharedMemoryRawUAVStart
                         : SystemBindlessView ::kSharedMemoryRawSRVAndNullRawUAVStart));
      } else {
        gpu_handle_shared_memory_and_bindful_edram = current_shared_memory_binding_is_uav_.value()
                                                         ? gpu_handle_shared_memory_uav_and_edram_
                                                         : gpu_handle_shared_memory_srv_and_edram_;
      }
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
          root_parameter_shared_memory_and_bindful_edram, gpu_handle_shared_memory_and_bindful_edram);
    }
    current_graphics_root_up_to_date_ |= 1u << root_parameter_shared_memory_and_bindful_edram;
  }
  if (bindless_resources_used_) {
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesPixel))) {
      if (native_terrain) {
        deferred_command_list_.D3DSetGraphicsRootShaderResourceView(
            kRootParameter_Bindless_DescriptorIndicesPixel,
            terrain_addresses[1] ? terrain_addresses[1] : shared_memory_->GetGPUAddress());
      } else if (current_graphics_root_signature_ == root_signature_fh1_layered_) {
        assert_true(texture_count_pixel == 2 && sampler_count_pixel == 1);
        deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
            kRootParameter_Bindless_DescriptorIndicesPixel,
            provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_,
                fh1_fixed_descriptor_indices_[1]));
      } else {
        deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
            kRootParameter_Bindless_DescriptorIndicesPixel,
            cbuffer_binding_descriptor_indices_pixel_.address);
      }
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesPixel;
    }
    if (!native_layered && !(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesVertex))) {
      if (native_terrain || native_skinned) {
        deferred_command_list_.D3DSetGraphicsRootShaderResourceView(
            kRootParameter_Bindless_DescriptorIndicesVertex,
            terrain_addresses[0] ? terrain_addresses[0] : shared_memory_->GetGPUAddress());
      } else {
        deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
            kRootParameter_Bindless_DescriptorIndicesVertex,
            cbuffer_binding_descriptor_indices_vertex_.address);
      }
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesVertex;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_SamplerHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_SamplerHeap,
          current_graphics_root_signature_ == root_signature_fh1_layered_
              ? provider.OffsetSamplerDescriptor(sampler_bindless_heap_gpu_start_,
                                                  fh1_fixed_descriptor_indices_[2])
              : sampler_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_SamplerHeap;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_ViewHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_ViewHeap,
          current_graphics_root_signature_ == root_signature_fh1_layered_
              ? provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_,
                    fh1_fixed_descriptor_indices_[0])
              : view_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_ViewHeap;
    }
  } else {
    uint32_t extra_index;
    extra_index = current_graphics_root_bindful_extras_.textures_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.textures_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
  }

  return true;
}

bool D3D12CommandProcessor::InitializeNativeGuestOutputGpuTiming() {
  native_guest_output_gpu_query_heap_.Reset();
  native_guest_output_gpu_query_readback_.Reset();
  native_guest_output_gpu_query_mapping_ = nullptr;
  native_guest_output_gpu_timestamp_frequency_ = 0;
  native_guest_output_gpu_timing_slots_ = {};
  native_guest_output_gpu_timing_active_ = false;
  fh1_gpu_pass_timing_slots_ = {};
  fh1_gpu_pass_timing_active_ = {};
  fh1_gpu_pass_timing_aggregates_.clear();
  fh1_gpu_pass_family_timing_aggregates_.clear();
  fh1_gpu_pass_timing_drops_ = 0;
  fh1_gpu_pass_timing_busy_drops_ = 0;
  fh1_gpu_pass_timing_capacity_drops_ = 0;
  fh1_gpu_pass_timing_interrupted_drops_ = 0;
  fh1_gpu_pass_timing_invalid_drops_ = 0;
  fh1_gpu_pass_timing_last_log_frame_ = 0;

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
  if (!device || !direct_queue ||
      FAILED(direct_queue->GetTimestampFrequency(
          &native_guest_output_gpu_timestamp_frequency_)) ||
      !native_guest_output_gpu_timestamp_frequency_) {
    REXGPU_WARN("Native guest-output GPU timestamps are unavailable");
    return false;
  }

  D3D12_QUERY_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  heap_desc.Count =
      kQueueFrames * kGpuTimingQueriesPerFrame;
  if (FAILED(device->CreateQueryHeap(
          &heap_desc,
          IID_PPV_ARGS(&native_guest_output_gpu_query_heap_)))) {
    REXGPU_WARN("Failed to create native guest-output GPU timestamp heap");
    return false;
  }

  const uint64_t readback_size =
      sizeof(uint64_t) * heap_desc.Count;
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      buffer_desc, readback_size, D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          GetD3D12Provider().GetHeapFlagCreateNotZeroed(), &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&native_guest_output_gpu_query_readback_)))) {
    REXGPU_WARN(
        "Failed to allocate native guest-output GPU timestamp readback");
    native_guest_output_gpu_query_heap_.Reset();
    return false;
  }

  D3D12_RANGE read_range{0, SIZE_T(readback_size)};
  void* mapping = nullptr;
  if (FAILED(native_guest_output_gpu_query_readback_->Map(
          0, &read_range, &mapping))) {
    REXGPU_WARN("Failed to map native guest-output GPU timestamps");
    native_guest_output_gpu_query_readback_.Reset();
    native_guest_output_gpu_query_heap_.Reset();
    return false;
  }
  native_guest_output_gpu_query_mapping_ =
      static_cast<uint64_t*>(mapping);
  native_guest_output_gpu_timing_active_ = true;
  return true;
}

void D3D12CommandProcessor::ShutdownNativeGuestOutputGpuTiming() {
  RetireNativeGuestOutputGpuTimings();
  LogFh1GpuPassTimings();
  if (native_guest_output_gpu_query_readback_ &&
      native_guest_output_gpu_query_mapping_) {
    native_guest_output_gpu_query_readback_->Unmap(0, nullptr);
  }
  native_guest_output_gpu_query_mapping_ = nullptr;
  native_guest_output_gpu_query_readback_.Reset();
  native_guest_output_gpu_query_heap_.Reset();
  native_guest_output_gpu_timestamp_frequency_ = 0;
  native_guest_output_gpu_timing_slots_ = {};
  native_guest_output_gpu_timing_active_ = false;
  fh1_gpu_pass_timing_slots_ = {};
  fh1_gpu_pass_timing_active_ = {};
  fh1_gpu_pass_timing_aggregates_.clear();
  fh1_gpu_pass_family_timing_aggregates_.clear();
}

void D3D12CommandProcessor::LogFh1GpuPassTimings() {
  std::vector<std::pair<uint64_t, Fh1GpuPassTimingAggregate>> ranked(
      fh1_gpu_pass_timing_aggregates_.begin(),
      fh1_gpu_pass_timing_aggregates_.end());
  std::sort(ranked.begin(), ranked.end(), [](const auto& left,
                                             const auto& right) {
    return left.second.total_ns > right.second.total_ns;
  });
  for (size_t index = 0; index < std::min<size_t>(ranked.size(), 20);
       ++index) {
    const auto& [signature, timing] = ranked[index];
    REXGPU_INFO(
        "FH1 V4 pass timing {:016X}: samples {}, total {} ns, average {} "
        "ns, maximum {} ns",
        signature, timing.samples, timing.total_ns,
        timing.samples ? timing.total_ns / timing.samples : 0,
        timing.maximum_ns);
  }
  if (!ranked.empty() || fh1_gpu_pass_timing_drops_) {
    REXGPU_INFO("FH1 V4 pass timing signatures {}, dropped samples {}",
                ranked.size(), fh1_gpu_pass_timing_drops_);
    // Counts are loss events, not unique samples: interrupted endpoints may
    // subsequently retire as invalid records.
    REXGPU_INFO("FH1 timing loss reasons {{\"busy\":{},\"capacity\":{},"
                "\"interrupted\":{},\"invalid\":{}}}",
                fh1_gpu_pass_timing_busy_drops_,
                fh1_gpu_pass_timing_capacity_drops_,
                fh1_gpu_pass_timing_interrupted_drops_,
                fh1_gpu_pass_timing_invalid_drops_);
  }
  std::vector<std::pair<uint64_t, Fh1GpuPassTimingAggregate>> ranked_families(
      fh1_gpu_pass_family_timing_aggregates_.begin(),
      fh1_gpu_pass_family_timing_aggregates_.end());
  std::sort(ranked_families.begin(), ranked_families.end(),
            [](const auto& left, const auto& right) {
              return left.second.total_ns > right.second.total_ns;
            });
  for (size_t index = 0;
       index < std::min<size_t>(ranked_families.size(),
                                kFh1GpuPassTimingCapacity);
       ++index) {
    const auto& [family, timing] = ranked_families[index];
    REXGPU_INFO(
        "FH1 V5 pass family {:016X}: attachment {:016X}, first family "
        "{:016X}, first draw {:016X}, copy {:016X}, samples {}, draws "
        "{}-{} (average {}), total {} ns, average {} ns, maximum {} ns, "
        "average draw {} ns, average resolve {} ns, maximum resolve {} ns",
        family, timing.attachment_state, timing.first_draw_family,
        timing.first_draw_identity, timing.terminal_copy_state,
        timing.samples, timing.minimum_draw_count, timing.maximum_draw_count,
        timing.samples ? timing.total_draws / timing.samples : 0,
        timing.total_ns,
        timing.samples ? timing.total_ns / timing.samples : 0,
        timing.maximum_ns,
        timing.samples ? timing.total_draw_ns / timing.samples : 0,
        timing.samples ? timing.total_resolve_ns / timing.samples : 0,
        timing.maximum_resolve_ns);
  }
  if (!ranked_families.empty()) {
    REXGPU_INFO("FH1 V5 pass timing families {}", ranked_families.size());
  }
  if (fh1_tonemap_native_draws_) {
    REXGPU_INFO(
        "FH1 V5 native tone-map draws {}, equivalent Xenos draws removed {}",
        fh1_tonemap_native_draws_, fh1_tonemap_native_draws_);
  }
  if (fh1_velocity_dilate_native_draws_) {
    REXGPU_INFO(
        "FH1 V5 native velocity-dilate draws {}, equivalent Xenos draws "
        "removed {}",
        fh1_velocity_dilate_native_draws_,
        fh1_velocity_dilate_native_draws_);
  }
}

void D3D12CommandProcessor::BeginNativeGuestOutputGpuTimingFrame() {
  if (!native_guest_output_gpu_query_heap_) {
    return;
  }
  const uint32_t slot_index = uint32_t(frame_current_ % kQueueFrames);
  auto& fh1_slot = fh1_gpu_pass_timing_slots_[slot_index];
  if (!fh1_slot.submission) {
    fh1_slot = {};
    fh1_gpu_pass_timing_active_ = {};
  } else {
    ++fh1_gpu_pass_timing_drops_;
    ++fh1_gpu_pass_timing_busy_drops_;
  }
  if (!native_guest_output_gpu_timing_active_) {
    return;
  }
  auto& slot = native_guest_output_gpu_timing_slots_[slot_index];
  if (slot.submission) {
    PROFILE_NATIVE_GPU_TIMING_DROP();
    return;
  }
  slot = {};
  slot.frame_started = true;
  const uint32_t query_base =
      slot_index * kGpuTimingQueriesPerFrame;
  deferred_command_list_.D3DEndQuery(
      native_guest_output_gpu_query_heap_.Get(),
      D3D12_QUERY_TYPE_TIMESTAMP, query_base);
}

void D3D12CommandProcessor::EndNativeGuestOutputGpuTimingFrame() {
  if (!native_guest_output_gpu_timing_active_) {
    return;
  }
  const uint32_t slot_index = uint32_t(frame_current_ % kQueueFrames);
  auto& slot = native_guest_output_gpu_timing_slots_[slot_index];
  if (!slot.frame_started || slot.submission) {
    return;
  }
  const uint32_t query_base = slot_index * kGpuTimingQueriesPerFrame;
  deferred_command_list_.D3DEndQuery(
      native_guest_output_gpu_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
      query_base + 1);
  deferred_command_list_.D3DResolveQueryData(
      native_guest_output_gpu_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
      query_base, 2, native_guest_output_gpu_query_readback_.Get(),
      uint64_t(query_base) * sizeof(uint64_t));
  slot.guest_timed = true;
  slot.submission = submission_current_;
}

bool D3D12CommandProcessor::IsFh1GpuWorkTimingSampleFrame() const {
  return Fh1GpuCorpusEnabled() && native_guest_output_gpu_query_heap_ &&
         !(observation_frame_sequence_ % 60);
}

D3D12CommandProcessor::Fh1GpuWorkTiming
D3D12CommandProcessor::BeginFh1TextureLoadTiming(
    const D3D12TextureCache::TextureKey& key, bool force_sample) {
  if (!(force_sample && Fh1GpuCorpusEnabled() && native_guest_output_gpu_query_heap_) &&
      !IsFh1GpuWorkTimingSampleFrame()) {
    return {};
  }
  auto& slot = fh1_gpu_pass_timing_slots_[frame_current_ % kQueueFrames];
  if (slot.submission || slot.record_count >= kFh1GpuPassTimingCapacity) {
    ++fh1_gpu_pass_timing_drops_;
    if (slot.submission) {
      ++fh1_gpu_pass_timing_busy_drops_;
    } else {
      ++fh1_gpu_pass_timing_capacity_drops_;
    }
    return {};
  }
  const uint32_t index = slot.record_count++;
  auto& record = slot.records[index];
  record = {};
  record.texture_key = key;
  record.frame = observation_frame_sequence_;
  record.query_offset = kNativeGuestOutputGpuQueriesPerFrame + index * 3;
  // Initialize every query, including endpoints of any interrupted sample.
  // Normal completion overwrites the two endpoints before the slot is resolved.
  for (uint32_t point = 0; point < 3; ++point) {
    deferred_command_list_.D3DEndQuery(native_guest_output_gpu_query_heap_.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        uint32_t(frame_current_ % kQueueFrames) * kGpuTimingQueriesPerFrame +
            record.query_offset + point);
  }
  return {frame_current_, submission_current_, index};
}

D3D12CommandProcessor::Fh1GpuWorkTiming
D3D12CommandProcessor::BeginFh1RenderTargetTransferTiming(uint32_t transfer_count,
                                                       bool resolve_clear) {
  if (!transfer_count && !resolve_clear) {
    return {};
  }
  auto timing = BeginFh1TextureLoadTiming({});
  if (timing.record != UINT32_MAX) {
    auto& record = fh1_gpu_pass_timing_slots_[timing.frame % kQueueFrames].records[timing.record];
    record.render_target_transfer = true;
    record.transfer_count = transfer_count;
    record.resolve_clear = resolve_clear;
  }
  return timing;
}

void D3D12CommandProcessor::FinishFh1RenderTargetTransferTiming(
    const Fh1GpuWorkTiming& timing) {
  AdvanceFh1GpuWorkTiming(timing, false);
  AdvanceFh1GpuWorkTiming(timing, true);
}

void D3D12CommandProcessor::AdvanceFh1GpuWorkTiming(
    const Fh1GpuWorkTiming& timing, bool finish) {
  if (timing.record == UINT32_MAX) {
    return;
  }
  auto& slot = fh1_gpu_pass_timing_slots_[frame_current_ % kQueueFrames];
  if (timing.frame != frame_current_ || timing.submission != submission_current_ ||
      slot.submission || timing.record >= slot.record_count) {
    ++fh1_gpu_pass_timing_drops_;
    ++fh1_gpu_pass_timing_interrupted_drops_;
    return;
  }
  auto& record = slot.records[timing.record];
  deferred_command_list_.D3DEndQuery(native_guest_output_gpu_query_heap_.Get(),
      D3D12_QUERY_TYPE_TIMESTAMP,
      uint32_t(frame_current_ % kQueueFrames) * kGpuTimingQueriesPerFrame +
          record.query_offset + (finish ? 2 : 1));
  record.work_complete = finish;
}

void D3D12CommandProcessor::ObserveFh1GpuPassTimingDraw(
    const system::GraphicsFh1ExecutionKey& key, uint64_t frame,
    uint64_t prepare_cpu_time_ns) {
  if (!Fh1GpuCorpusEnabled() ||
      !native_guest_output_gpu_query_heap_ ||
      key.kind != system::GraphicsFh1ExecutionKind::kDraw || frame % 60) {
    return;
  }
  if (fh1_gpu_pass_timing_active_.draw_count &&
      (fh1_gpu_pass_timing_active_.frame != frame ||
       fh1_gpu_pass_timing_active_.attachment_state !=
           key.attachment_state)) {
    FinishFh1GpuPassTiming(0);
  }
  uint64_t draw_family = UINT64_C(0xCBF29CE484222325);
  draw_family = HashFh1ExecutionValue(draw_family, key.shader_state);
  draw_family = HashFh1ExecutionValue(draw_family, key.pipeline_state);
  draw_family = HashFh1ExecutionValue(draw_family, key.operation_state);
  if (!fh1_gpu_pass_timing_active_.draw_count) {
    fh1_gpu_pass_timing_active_.frame = frame;
    fh1_gpu_pass_timing_active_.slot_index =
        uint32_t(frame_current_ % kQueueFrames);
    fh1_gpu_pass_timing_active_.attachment_state = key.attachment_state;
    fh1_gpu_pass_timing_active_.first_draw_family = draw_family;
    fh1_gpu_pass_timing_active_.first_draw_identity = key.identity;
    fh1_gpu_pass_timing_active_.running_signature =
        UINT64_C(0xCBF29CE484222325);
    fh1_gpu_pass_timing_active_.running_signature = HashFh1ExecutionValue(
        fh1_gpu_pass_timing_active_.running_signature,
        key.attachment_state);

    auto& slot = fh1_gpu_pass_timing_slots_[frame_current_ % kQueueFrames];
    if (!slot.submission &&
        slot.record_count < kFh1GpuPassTimingCapacity) {
      const uint32_t record_index = slot.record_count++;
      auto& record = slot.records[record_index];
      record = {};
      record.query_offset = kNativeGuestOutputGpuQueriesPerFrame +
                            record_index * 3;
      fh1_gpu_pass_timing_active_.record_index = record_index;
      record.begin_submission = submission_current_;
      record.recording_start_ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count());
      deferred_command_list_.D3DEndQuery(
          native_guest_output_gpu_query_heap_.Get(),
          D3D12_QUERY_TYPE_TIMESTAMP,
          uint32_t(frame_current_ % kQueueFrames) *
                  kGpuTimingQueriesPerFrame +
              record.query_offset);
    } else {
      ++fh1_gpu_pass_timing_drops_;
      if (slot.submission) {
        ++fh1_gpu_pass_timing_busy_drops_;
      } else {
        ++fh1_gpu_pass_timing_capacity_drops_;
      }
    }
  }
  fh1_gpu_pass_timing_active_.running_signature = HashFh1ExecutionValue(
      fh1_gpu_pass_timing_active_.running_signature, draw_family);
  fh1_gpu_pass_timing_active_.prepare_cpu_time_ns += prepare_cpu_time_ns;
  ++fh1_gpu_pass_timing_active_.draw_count;
  fh1_gpu_pass_timing_active_.hazard_flags |= key.hazard_flags;
}

void D3D12CommandProcessor::ObserveFh1GpuPassTimingCopy(
    const system::GraphicsFh1ExecutionKey& key, uint64_t frame) {
  if (key.kind != system::GraphicsFh1ExecutionKind::kCopyResolve ||
      !fh1_gpu_pass_timing_active_.draw_count ||
      fh1_gpu_pass_timing_active_.frame != frame) {
    return;
  }
  uint64_t terminal_copy_state = UINT64_C(0xCBF29CE484222325);
  terminal_copy_state = HashFh1ExecutionValue(terminal_copy_state,
                                               key.attachment_state);
  terminal_copy_state = HashFh1ExecutionValue(terminal_copy_state,
                                               key.operation_state);
  fh1_gpu_pass_timing_active_.hazard_flags |= key.hazard_flags;
  FinishFh1GpuPassTiming(terminal_copy_state);
}

void D3D12CommandProcessor::BeginFh1GpuPassTimingCopy() {
  if (!fh1_gpu_pass_timing_active_.draw_count ||
      fh1_gpu_pass_timing_active_.record_index == UINT32_MAX) {
    return;
  }
  auto& record = fh1_gpu_pass_timing_slots_[
                     fh1_gpu_pass_timing_active_.slot_index]
                     .records[fh1_gpu_pass_timing_active_.record_index];
  deferred_command_list_.D3DEndQuery(
      native_guest_output_gpu_query_heap_.Get(),
      D3D12_QUERY_TYPE_TIMESTAMP,
      fh1_gpu_pass_timing_active_.slot_index * kGpuTimingQueriesPerFrame +
          record.query_offset + 1);
  record.copy_started = true;
}

void D3D12CommandProcessor::FinishFh1GpuPassTiming(
    uint64_t terminal_copy_state) {
  if (!fh1_gpu_pass_timing_active_.draw_count) {
    return;
  }
  if (fh1_gpu_pass_timing_active_.record_index != UINT32_MAX) {
    auto& slot = fh1_gpu_pass_timing_slots_[
        fh1_gpu_pass_timing_active_.slot_index];
    auto& record =
        slot.records[fh1_gpu_pass_timing_active_.record_index];
    if (terminal_copy_state &&
        !fh1_gpu_pass_timing_active_.hazard_flags) {
      record.signature = HashFh1ExecutionValue(
          fh1_gpu_pass_timing_active_.running_signature,
          terminal_copy_state);
      if (!record.signature) {
        record.signature = 1;
      }
    }
    record.family = UINT64_C(0xCBF29CE484222325);
    record.family = HashFh1ExecutionValue(
        record.family, fh1_gpu_pass_timing_active_.attachment_state);
    record.family = HashFh1ExecutionValue(
        record.family, fh1_gpu_pass_timing_active_.first_draw_family);
    record.family = HashFh1ExecutionValue(
        record.family, fh1_gpu_pass_timing_active_.first_draw_identity);
    record.family = HashFh1ExecutionValue(record.family,
                                           terminal_copy_state);
    record.family = HashFh1ExecutionValue(
        record.family, fh1_gpu_pass_timing_active_.hazard_flags);
    if (!record.family) {
      record.family = 1;
    }
    record.prepare_cpu_time_ns = fh1_gpu_pass_timing_active_.prepare_cpu_time_ns;
    record.frame = fh1_gpu_pass_timing_active_.frame;
    record.attachment_state = fh1_gpu_pass_timing_active_.attachment_state;
    record.first_draw_family =
        fh1_gpu_pass_timing_active_.first_draw_family;
    record.first_draw_identity =
        fh1_gpu_pass_timing_active_.first_draw_identity;
    record.terminal_copy_state = terminal_copy_state;
    record.draw_count = fh1_gpu_pass_timing_active_.draw_count;
    record.end_submission = submission_current_;
    record.recording_wall_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) -
        record.recording_start_ns;
    if (!record.copy_started) {
      deferred_command_list_.D3DEndQuery(
          native_guest_output_gpu_query_heap_.Get(),
          D3D12_QUERY_TYPE_TIMESTAMP,
          fh1_gpu_pass_timing_active_.slot_index * kGpuTimingQueriesPerFrame +
              record.query_offset + 1);
    }
    deferred_command_list_.D3DEndQuery(
        native_guest_output_gpu_query_heap_.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        fh1_gpu_pass_timing_active_.slot_index * kGpuTimingQueriesPerFrame +
            record.query_offset + 2);
  }
  fh1_gpu_pass_timing_active_ = {};
}

void D3D12CommandProcessor::EndFh1GpuPassTimingFrame() {
  if (!Fh1GpuCorpusEnabled() ||
      !native_guest_output_gpu_query_heap_ ||
      !native_guest_output_gpu_query_readback_) {
    return;
  }
  FinishFh1GpuPassTiming(0);
  const uint32_t slot_index = uint32_t(frame_current_ % kQueueFrames);
  auto& slot = fh1_gpu_pass_timing_slots_[slot_index];
  if (!slot.record_count || slot.submission) {
    return;
  }
  const uint32_t first_query =
      slot_index * kGpuTimingQueriesPerFrame +
      kNativeGuestOutputGpuQueriesPerFrame;
  deferred_command_list_.D3DResolveQueryData(
      native_guest_output_gpu_query_heap_.Get(),
      D3D12_QUERY_TYPE_TIMESTAMP, first_query, slot.record_count * 3,
      native_guest_output_gpu_query_readback_.Get(),
      uint64_t(first_query) * sizeof(uint64_t));
  slot.submission = submission_current_;
}

void D3D12CommandProcessor::RetireNativeGuestOutputGpuTimings() {
  if (!native_guest_output_gpu_query_mapping_ ||
      !native_guest_output_gpu_timestamp_frequency_) {
    return;
  }
  auto ticks_to_ns = [this](uint64_t ticks) -> int64_t {
    const long double nanoseconds =
        static_cast<long double>(ticks) * 1000000000.0L /
        static_cast<long double>(
            native_guest_output_gpu_timestamp_frequency_);
    return static_cast<int64_t>(nanoseconds);
  };
  for (uint32_t slot_index = 0; slot_index < kQueueFrames;
       ++slot_index) {
    auto& slot = native_guest_output_gpu_timing_slots_[slot_index];
    if (!slot.submission || slot.submission > submission_completed_) {
      continue;
    }
    const uint32_t query_base =
        slot_index * kGpuTimingQueriesPerFrame;
    const uint64_t* timestamps =
        native_guest_output_gpu_query_mapping_ + query_base;
    const bool valid = slot.guest_timed && timestamps[1] >= timestamps[0];
    if (valid) {
      PROFILE_GUEST_FRAME_GPU_TIME_NS(
          ticks_to_ns(timestamps[1] - timestamps[0]));
      PROFILE_GUEST_FRAME_GPU_TIMING_SAMPLE();
    } else {
      PROFILE_NATIVE_GPU_TIMING_DROP();
    }
    slot = {};
  }
  for (uint32_t slot_index = 0; slot_index < kQueueFrames;
       ++slot_index) {
    auto& slot = fh1_gpu_pass_timing_slots_[slot_index];
    if (!slot.submission || slot.submission > submission_completed_) {
      continue;
    }
    const uint32_t query_base =
        slot_index * kGpuTimingQueriesPerFrame;
    const uint64_t* timestamps =
        native_guest_output_gpu_query_mapping_ + query_base;
    for (uint32_t record_index = 0; record_index < slot.record_count;
         ++record_index) {
      const auto& record = slot.records[record_index];
      const uint64_t start = timestamps[record.query_offset];
      const uint64_t copy_start = timestamps[record.query_offset + 1];
      const uint64_t end = timestamps[record.query_offset + 2];
      if (record.render_target_transfer) {
        if (record.work_complete && start <= copy_start && copy_start <= end) {
          REXGPU_INFO("FH1 render target transfer sample {{\"frame\":{},"
                      "\"submission\":{},\"record\":{},\"transfers\":{},"
                      "\"resolve_clear\":{},\"total_ns\":{}}}",
                      record.frame, slot.submission, record_index,
                      record.transfer_count, record.resolve_clear,
                      ticks_to_ns(end - start));
        } else {
          ++fh1_gpu_pass_timing_drops_;
          ++fh1_gpu_pass_timing_invalid_drops_;
        }
        continue;
      }
      if (record.texture_key.is_valid) {
        if (record.work_complete && start <= copy_start && copy_start <= end) {
          const auto& key = record.texture_key;
          REXGPU_INFO(
              "FH1 texture load sample {{\"frame\":{},\"submission\":{},\"record\":{},"
              "\"base\":\"{:08X}\",\"width\":{},\"height\":{},\"format\":{},"
              "\"pitch\":{},\"packed\":{},\"endian\":{},\"signed\":{},"
              "\"conversion_ns\":{},\"copy_ns\":{}}}",
              record.frame, slot.submission, record_index, uint32_t(key.base_page << 12),
              key.GetWidth(), key.GetHeight(), uint32_t(key.format), uint32_t(key.pitch << 5),
              uint32_t(key.packed_mips), uint32_t(key.endianness), uint32_t(key.signed_separate),
              ticks_to_ns(copy_start - start), ticks_to_ns(end - copy_start));
        } else {
          ++fh1_gpu_pass_timing_drops_;
          ++fh1_gpu_pass_timing_invalid_drops_;
        }
        continue;
      }
      if (!record.family || copy_start < start || end < copy_start) {
        ++fh1_gpu_pass_timing_drops_;
        ++fh1_gpu_pass_timing_invalid_drops_;
        continue;
      }
      const uint64_t duration_ns = uint64_t(ticks_to_ns(end - start));
      const uint64_t draw_ns = uint64_t(ticks_to_ns(copy_start - start));
      const uint64_t resolve_ns =
          record.copy_started ? uint64_t(ticks_to_ns(end - copy_start)) : 0;
      REXGPU_INFO(
          "FH1 V5 pass sample {{\"frame\":{},\"submission\":{},\"record\":{},"
          "\"family\":\"{:016X}\",\"first_draw\":\"{:016X}\",\"draws\":{},"
          "\"total_ns\":{},\"draw_ns\":{},\"resolve_ns\":{},\"prepare_cpu_ns\":{},"
          "\"recording_wall_ns\":{},\"begin_submission\":{},\"end_submission\":{}}}",
          record.frame, slot.submission, record_index, record.family,
          record.first_draw_identity, record.draw_count, duration_ns, draw_ns,
          resolve_ns, record.prepare_cpu_time_ns, record.recording_wall_ns,
          record.begin_submission, record.end_submission);
      if (record.signature) {
        auto& aggregate = fh1_gpu_pass_timing_aggregates_[record.signature];
        ++aggregate.samples;
        aggregate.total_ns += duration_ns;
        aggregate.maximum_ns = std::max(aggregate.maximum_ns, duration_ns);
        aggregate.total_draw_ns += draw_ns;
        aggregate.total_resolve_ns += resolve_ns;
        aggregate.maximum_resolve_ns =
            std::max(aggregate.maximum_resolve_ns, resolve_ns);
      }
      auto& family = fh1_gpu_pass_family_timing_aggregates_[record.family];
      if (!family.samples) {
        family.attachment_state = record.attachment_state;
        family.first_draw_family = record.first_draw_family;
        family.first_draw_identity = record.first_draw_identity;
        family.terminal_copy_state = record.terminal_copy_state;
      }
      ++family.samples;
      family.total_ns += duration_ns;
      family.maximum_ns = std::max(family.maximum_ns, duration_ns);
      family.total_draw_ns += draw_ns;
      family.total_resolve_ns += resolve_ns;
      family.maximum_resolve_ns =
          std::max(family.maximum_resolve_ns, resolve_ns);
      family.total_draws += record.draw_count;
      family.minimum_draw_count =
          std::min(family.minimum_draw_count, record.draw_count);
      family.maximum_draw_count =
          std::max(family.maximum_draw_count, record.draw_count);
    }
    slot = {};
  }
  if (frame_current_ >= fh1_gpu_pass_timing_last_log_frame_ + 600) {
    fh1_gpu_pass_timing_last_log_frame_ = frame_current_;
    LogFh1GpuPassTimings();
  }
}

bool D3D12CommandProcessor::InitializeOcclusionQueryResources() {
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  zpd_lifecycle_.Reset();
  modern_occlusion_query_free_indices_.clear();
  modern_occlusion_query_generations_.clear();
  modern_occlusion_queries_pending_.clear();
  modern_occlusion_query_active_index_ = UINT32_MAX;
  modern_occlusion_query_active_generation_ = 0;
  modern_occlusion_query_active_report_ = ZPDLifecycle::kInvalidReportHandle;
  occlusion_query_resources_available_ = false;
  occlusion_query_heap_.Reset();
  occlusion_query_readback_.Reset();
  occlusion_query_readback_mapping_ = nullptr;

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (!device) {
    return false;
  }

  D3D12_QUERY_HEAP_DESC heap_desc;
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = kMaxOcclusionQueries;
  heap_desc.NodeMask = 0;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&occlusion_query_heap_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to create occlusion query heap, using fake sample counts");
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sizeof(uint64_t) * kMaxOcclusionQueries,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                             GetD3D12Provider().GetHeapFlagCreateNotZeroed(),
                                             &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&occlusion_query_readback_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate occlusion query readback buffer, using fake "
        "sample counts");
    occlusion_query_heap_.Reset();
    return false;
  }

  D3D12_RANGE read_range = {0, sizeof(uint64_t) * kMaxOcclusionQueries};
  void* mapping = nullptr;
  if (FAILED(occlusion_query_readback_->Map(0, &read_range, &mapping))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    occlusion_query_readback_.Reset();
    occlusion_query_heap_.Reset();
    return false;
  }

  occlusion_query_readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
  modern_occlusion_query_generations_.resize(kMaxOcclusionQueries, 0);
  modern_occlusion_query_free_indices_.reserve(kMaxOcclusionQueries);
  for (uint32_t i = kMaxOcclusionQueries; i != 0; --i) {
    modern_occlusion_query_free_indices_.push_back(i - 1);
  }
  occlusion_query_resources_available_ = true;
  return true;
}

void D3D12CommandProcessor::ShutdownOcclusionQueryResources() {
  if (modern_occlusion_query_active_index_ != UINT32_MAX && submission_open_ &&
      occlusion_query_heap_ && occlusion_query_readback_) {
    CloseModernZPDSegment();
    EndSubmission(false);
  }
  DisableHostOcclusionQueries();

  zpd_lifecycle_.Reset();
  modern_occlusion_query_free_indices_.clear();
  modern_occlusion_query_generations_.clear();
  modern_occlusion_queries_pending_.clear();
  modern_occlusion_query_active_index_ = UINT32_MAX;
  modern_occlusion_query_active_generation_ = 0;
  modern_occlusion_query_active_report_ = ZPDLifecycle::kInvalidReportHandle;

  if (occlusion_query_readback_ && occlusion_query_readback_mapping_) {
    occlusion_query_readback_->Unmap(0, nullptr);
  }
  occlusion_query_readback_mapping_ = nullptr;
  occlusion_query_readback_.Reset();
  occlusion_query_heap_.Reset();
}

bool D3D12CommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  host_index_out = occlusion_query_cursor_++;
  return true;
}

void D3D12CommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_heap_) {
    uint32_t host_index = active_occlusion_query_.host_index;
    // Clear before EndSubmission to prevent the EndSubmission safety net from issuing a second
    // EndQuery for the same index.
    active_occlusion_query_ = {};
    if (BeginSubmission(true)) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         host_index);
      EndSubmission(false);
    }
  } else {
    active_occlusion_query_ = {};
  }
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       host_index);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

bool D3D12CommandProcessor::EndGuestOcclusionQuery(
    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      !active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {
    return false;
  }

  uint32_t host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                     host_index);
  deferred_command_list_.D3DResolveQueryData(
      occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, host_index, 1,
      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);

  if (!EndSubmission(false)) {
    return false;
  }

  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;
  if (!CheckSubmissionFence(query_submission)) {
    return false;
  }
  if (!occlusion_query_readback_mapping_) {
    return false;
  }

  uint64_t samples = occlusion_query_readback_mapping_[host_index];
  samples = NormalizeOcclusionSamples(samples);
  WriteGuestOcclusionResult(sample_counts, samples);
  return true;
}

uint64_t D3D12CommandProcessor::NormalizeOcclusionSamples(uint64_t samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  uint64_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint64_t scale_y = texture_cache_->draw_resolution_scale_y();
  uint64_t scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void D3D12CommandProcessor::WriteGuestOcclusionResult(
    xenos::xe_gpu_depth_sample_counts* sample_counts, uint64_t samples) {
  if (!sample_counts) {
    return;
  }
  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

bool D3D12CommandProcessor::AcquireModernOcclusionQuery(uint32_t& host_index_out,
                                                        uint32_t& generation_out) {
  RetireModernZPDQueries();
  if (modern_occlusion_query_free_indices_.empty()) {
    return false;
  }
  host_index_out = modern_occlusion_query_free_indices_.back();
  modern_occlusion_query_free_indices_.pop_back();
  generation_out = ++modern_occlusion_query_generations_[host_index_out];
  if (!generation_out) {
    generation_out = ++modern_occlusion_query_generations_[host_index_out];
  }
  return true;
}

void D3D12CommandProcessor::ReleaseModernOcclusionQuery(uint32_t host_index, uint32_t generation) {
  if (host_index >= modern_occlusion_query_generations_.size() ||
      modern_occlusion_query_generations_[host_index] != generation) {
    return;
  }
  modern_occlusion_query_free_indices_.push_back(host_index);
}

bool D3D12CommandProcessor::OpenModernZPDSegment() {
  if (!submission_open_ || !occlusion_query_heap_ ||
      modern_occlusion_query_active_index_ != UINT32_MAX ||
      !zpd_lifecycle_.active().logical_active || !zpd_lifecycle_.active().segment_pending_begin) {
    return false;
  }
  uint32_t index = UINT32_MAX;
  uint32_t generation = 0;
  if (!AcquireModernOcclusionQuery(index, generation)) {
    return false;
  }
  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       index);
  modern_occlusion_query_active_index_ = index;
  modern_occlusion_query_active_generation_ = generation;
  modern_occlusion_query_active_report_ = zpd_lifecycle_.active().report_handle;
  zpd_lifecycle_.SegmentOpened();
  return true;
}

bool D3D12CommandProcessor::CloseModernZPDSegment() {
  if (modern_occlusion_query_active_index_ == UINT32_MAX || !submission_open_ ||
      !occlusion_query_heap_ || !occlusion_query_readback_) {
    return false;
  }
  uint32_t index = modern_occlusion_query_active_index_;
  uint32_t generation = modern_occlusion_query_active_generation_;
  ZPDLifecycle::ReportHandle handle = modern_occlusion_query_active_report_;
  modern_occlusion_query_active_index_ = UINT32_MAX;
  modern_occlusion_query_active_generation_ = 0;
  modern_occlusion_query_active_report_ = ZPDLifecycle::kInvalidReportHandle;

  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                     index);
  deferred_command_list_.D3DResolveQueryData(
      occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, index, 1,
      occlusion_query_readback_.Get(), sizeof(uint64_t) * index);
  modern_occlusion_queries_pending_.push_back({submission_current_, index, generation, handle});
  zpd_lifecycle_.SegmentClosed(submission_current_);
  PERF_counter_inc(kZpdReportSegments);
  return true;
}

void D3D12CommandProcessor::RetireModernZPDQueries() {
  if (!submission_fence_ || modern_occlusion_queries_pending_.empty()) {
    return;
  }
  submission_completed_ = std::max(submission_completed_, submission_fence_->GetCompletedValue());
  while (!modern_occlusion_queries_pending_.empty() &&
         modern_occlusion_queries_pending_.front().submission <= submission_completed_) {
    PendingModernOcclusionQuery pending = modern_occlusion_queries_pending_.front();
    modern_occlusion_queries_pending_.pop_front();
    bool generation_matches =
        pending.host_index < modern_occlusion_query_generations_.size() &&
        modern_occlusion_query_generations_[pending.host_index] == pending.generation;
    uint64_t samples = generation_matches && occlusion_query_readback_mapping_
                           ? occlusion_query_readback_mapping_[pending.host_index]
                           : 0;
    uint32_t delta = 0;
    bool commit = false;
    ZPDLifecycle::Report report;
    bool resolved =
        generation_matches &&
        zpd_lifecycle_.Resolve(pending.report_handle, samples,
                               texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1,
                               texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1,
                               delta, commit, report);
    if (!resolved || !commit) {
      if (!zpd_lifecycle_.Find(pending.report_handle)) {
        PERF_counter_inc(kZpdStaleResultRejections);
      }
    } else {
      WriteModernZPDReport(report, delta);
      PERF_counter_inc(kZpdAsyncResultPatches);
    }
    ReleaseModernOcclusionQuery(pending.host_index, pending.generation);
  }
}

bool D3D12CommandProcessor::AwaitModernZPDReport(ZPDLifecycle::ReportHandle report_handle,
                                                 uint32_t timeout_ms) {
  if (!zpd_lifecycle_.Find(report_handle)) {
    return true;
  }
  if (submission_open_) {
    EndSubmission(false);
  }
  PERF_counter_inc(kZpdStrictWaits);
  auto wait_start = std::chrono::steady_clock::now();
  uint64_t wait_submission = 0;
  for (const PendingModernOcclusionQuery& pending : modern_occlusion_queries_pending_) {
    if (pending.report_handle == report_handle) {
      wait_submission = std::max(wait_submission, pending.submission);
    }
  }
  bool signaled = wait_submission == 0 || submission_completed_ >= wait_submission;
  if (!signaled && submission_fence_ &&
      SUCCEEDED(
          submission_fence_->SetEventOnCompletion(wait_submission, fence_completion_event_))) {
    signaled = WaitForSingleObject(fence_completion_event_, timeout_ms) == WAIT_OBJECT_0;
  }
  auto elapsed = std::chrono::steady_clock::now() - wait_start;
  PERF_counter_add(kZpdStrictWaitTimeNs,
                   std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  if (signaled) {
    RetireModernZPDQueries();
    return zpd_lifecycle_.Find(report_handle) == nullptr;
  }

  ZPDLifecycle::Report report;
  bool commit = false;
  uint32_t fallback = 1;
  if (const ZPDLifecycle::Report* pending = zpd_lifecycle_.Find(report_handle)) {
    fallback = pending->has_cached_delta ? pending->cached_delta : 1;
  }
  if (zpd_lifecycle_.Abandon(report_handle, fallback, report, commit) && commit) {
    WriteModernZPDReport(report, fallback);
  }
  PERF_counter_inc(kZpdRetireTimeouts);
  return false;
}

void D3D12CommandProcessor::WriteModernZPDReport(const ZPDLifecycle::Report& report,
                                                 uint32_t delta) {
  auto* begin =
      report.begin_record
          ? memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(report.begin_record)
          : nullptr;
  auto* end =
      report.end_record
          ? memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(report.end_record)
          : nullptr;
  XenosZPDReport::WriteReportDelta(begin, end, report.begin_value, delta, begin && begin != end);
}

void D3D12CommandProcessor::WriteGammaRampSRV(bool is_pwl,
                                              D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  if (is_pwl) {
    desc.Format = DXGI_FORMAT_R16G16_UINT;
    desc.Buffer.FirstElement = 256 * 4 / 4;
    desc.Buffer.NumElements = 128 * 3;
  } else {
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 256;
  }
  device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &desc, handle);
}

}  // namespace rex::graphics::d3d12
