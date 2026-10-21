// NA-2.4 owned draw programs and NA-2.3 index versions (Pinyon Shift
// docs/NEAR_NATIVE_RESEARCH.md, issues #752 and #753). See the comment on
// ProgramDraw in vulkan/command_processor.h; the draw side is in
// VulkanCommandProcessor::IssueDrawImpl.

#include <algorithm>
#include <chrono>
#include <cstring>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DECLARE(int32_t, gpu_d3d_program);
REXCVAR_DECLARE(bool, gpu_draw_cost_model);
REXCVAR_DECLARE(double, fh1_hud_squeeze);
REXCVAR_DECLARE(double, fh1_hud_scale);
REXCVAR_DECLARE(double, fh1_hud_safe_zone);
REXCVAR_DECLARE(bool, fh1_hud_scale_suspended);

namespace rex::graphics::vulkan {

namespace {
// Programs not run for this many swaps are dropped at the log.
constexpr uint32_t kProgramSwaps = 600;
constexpr VkDeviceSize kIndexVersionAlignment = 256;
}  // namespace

void VulkanCommandProcessor::DrawProgramBegin(uint64_t key) {
  draw_program_ = nullptr;
  if (!draw_program_mode_) return;
  // Timed like the draws, so the complete cost of a mode includes it.
  const bool timed = REXCVAR_GET(gpu_draw_cost_model);
  const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  const uint32_t mode = std::min<uint32_t>(draw_program_mode_, 4);
  DrawProgramStats& s = draw_program_stats_;
  ++s.runs;
  // A placeholder marks the run. Timed only (4) looks nothing up; the other
  // modes look the program up at the run's first draw that reaches the
  // backend (DrawProgramResolve), as many runs draw nothing (later tiles).
  draw_program_ = &draw_program_timed_;
  draw_program_draw_ = 0;
  draw_program_pending_ = mode != 4;
  if (draw_program_pending_) {
    // Each pass that runs the list starts it in its own state: one program
    // each.
    const uint64_t entry =
        StateMemoKey() ^
        uint64_t(uintptr_t(active_vertex_shader())) * UINT64_C(0x9E3779B97F4A7C15) ^
        uint64_t(uintptr_t(active_pixel_shader())) * UINT64_C(0xC2B2AE3D27D4EB4F);
    draw_program_pending_key_ = (key * UINT64_C(0xBF58476D1CE4E5B9)) ^ entry;
  }
  if (timed) {
    s.begin_ns[mode] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
            .count());
  }
}

void VulkanCommandProcessor::DrawProgramResolve() {
  draw_program_pending_ = false;
  const uint64_t program_key = draw_program_pending_key_;
  // A direct-mapped cache in front of the map (cleared when programs are
  // evicted, so its pointers stay valid).
  ProgramCacheEntry& cached =
      draw_program_cache_[(program_key ^ (program_key >> 29)) & (kProgramCacheSize - 1)];
  DrawProgram* program = cached.key == program_key ? cached.program : nullptr;
  if (!program) {
    auto [it, inserted] = draw_programs_.try_emplace(program_key);
    draw_program_stats_.programs_made += inserted;
    program = &it->second;
    cached = {program_key, program};
  }
  program->last_swap = draw_program_swaps_;
  ++program->runs;
  // The settings the derivation reads, once a run instead of by draw.
  const uint32_t resolution_scale = texture_cache_->draw_resolution_scale_x() |
                                    texture_cache_->draw_resolution_scale_y() << 16;
  const double hud_squeeze = REXCVAR_GET(fh1_hud_squeeze);
  const double hud_scale =
      REXCVAR_GET(fh1_hud_scale_suspended)
          ? 1.0
          : REXCVAR_GET(fh1_hud_scale) * (1.0 - 2.0 * REXCVAR_GET(fh1_hud_safe_zone));
  const bool object_motion = fh1_native_executor_ && fh1_native_executor_->ObjectMotionEnabled();
  if (program->resolution_scale != resolution_scale || program->hud_squeeze != hud_squeeze ||
      program->hud_scale != hud_scale || program->object_motion != object_motion) {
    if (!program->draws.empty()) {
      for (const ProgramDraw& draw : program->draws) {
        if (draw.valid && draw.index_version != UINT32_MAX) RetireIndexVersion(draw.index_version);
      }
      program->draws.clear();
      ++draw_program_stats_.miss[kMissSettings];
    }
    program->resolution_scale = resolution_scale;
    program->hud_squeeze = hud_squeeze;
    program->hud_scale = hud_scale;
    program->object_motion = object_motion;
  }
  draw_program_object_motion_ = object_motion;
  draw_program_ = program;
}

void VulkanCommandProcessor::DrawProgramEnd() {
  draw_program_ = nullptr;
  draw_program_pending_ = false;
}

void VulkanCommandProcessor::DrawProgramRecord(ProgramDraw& d, bool recordable, bool compare,
                                               const ProgramDrawOutputs& o) {
  DrawProgramStats& s = draw_program_stats_;
  const PrimitiveProcessor::ProcessingResult& r = *o.primitive;
  using IndexType = PrimitiveProcessor::ProcessedIndexBufferType;
  // Converted index buffers live for one frame, and a DMA builtin buffer
  // needs its guest range requested: neither replays.
  const bool replayable_indices =
      r.index_buffer_type == IndexType::kNone ||
      r.index_buffer_type == IndexType::kHostBuiltinForAuto ||
      (r.index_buffer_type == IndexType::kGuestDMA && o.indices_owned &&
       o.index_version != UINT32_MAX);
  if (!recordable || !replayable_indices) {
    d.valid = false;
    ++s.unrecordable;
    return;
  }
  if (compare) {
    ++s.shadow_compared;
    const PrimitiveProcessor::ProcessingResult& p = d.primitive;
    const char* differs = nullptr;
    if (d.vertex_shader != o.vertex_shader || d.pixel_shader != o.pixel_shader) {
      differs = "shaders";
    } else if (d.primitive_polygonal != o.primitive_polygonal ||
               d.is_rasterization_done != o.is_rasterization_done ||
               d.depth_control.value != o.depth_control.value || d.color_mask != o.color_mask ||
               d.coverage_from_shader != draw_coverage_from_shader_) {
      differs = "analysis";
    } else if (p.guest_primitive_type != r.guest_primitive_type ||
               p.host_primitive_type != r.host_primitive_type ||
               p.host_vertex_shader_type != r.host_vertex_shader_type ||
               p.tessellation_mode != r.tessellation_mode ||
               p.guest_draw_vertex_count != r.guest_draw_vertex_count ||
               p.host_draw_vertex_count != r.host_draw_vertex_count ||
               p.line_loop_closing_index != r.line_loop_closing_index ||
               p.index_buffer_type != r.index_buffer_type ||
               p.guest_index_base != r.guest_index_base ||
               p.host_index_format != r.host_index_format ||
               p.host_shader_index_endian != r.host_shader_index_endian ||
               p.host_primitive_reset_enabled != r.host_primitive_reset_enabled ||
               (r.index_buffer_type == IndexType::kHostBuiltinForAuto &&
                p.host_index_buffer_handle != r.host_index_buffer_handle)) {
      differs = "primitive processing";
    } else if (d.vertex_modification.value != o.vertex_modification.value ||
               d.pixel_modification.value != o.pixel_modification.value ||
               d.vertex_translation != o.vertex_translation ||
               d.pixel_translation != o.pixel_translation) {
      differs = "translations";
    } else if (d.render_pass_key == o.render_pass_key &&
               d.pipeline_handle != o.pipeline_handle) {
      differs = "pipeline";
    } else if (std::memcmp(&d.viewport, o.viewport, sizeof(d.viewport))) {
      differs = "viewport";
    } else if (d.index_version != o.index_version) {
      differs = "index version";
    }
    if (d.index_version != UINT32_MAX) {
      // The version is live (the draw matched): the guest bytes must be the
      // ones copied, or the watch missed a write.
      const IndexVersion& version = index_versions_[d.index_version];
      const void* guest = memory_->TranslatePhysical(version.guest_base);
      if (version.verify_hash && guest &&
          XXH3_64bits(guest, version.bytes) != version.verify_hash) {
        ++s.shadow_index_mismatches;
        if (s.logs < 32) {
          ++s.logs;
          REXGPU_INFO("Draw programs: index version {:08X}+{} changed without its watch firing",
                      version.guest_base, version.bytes);
        }
      }
    }
    if (!differs) return;
    ++s.shadow_mismatches;
    if (s.logs < 32) {
      ++s.logs;
      REXGPU_INFO("Draw programs: shadow mismatch in {} (VS {:016X} PS {:016X}, {} indices)",
                  differs, o.vertex_shader ? o.vertex_shader->ucode_data_hash() : 0,
                  o.pixel_shader ? o.pixel_shader->ucode_data_hash() : 0, o.index_count);
    }
  }
  const RegisterFile& regs = *register_file_;
  d.memo_state = o.memo_state;
  d.active_vertex_shader = active_vertex_shader();
  d.active_pixel_shader = active_pixel_shader();
  d.prim_type = uint32_t(o.prim_type);
  d.index_count = o.index_count;
  d.initiator = regs.values[XE_GPU_REG_VGT_DRAW_INITIATOR];
  d.dma_base = o.indexed ? regs.values[XE_GPU_REG_VGT_DMA_BASE] : 0;
  d.dma_size = o.indexed ? regs.values[XE_GPU_REG_VGT_DMA_SIZE] : 0;
  d.resolution_scale = o.resolution_scale;
  d.hud_squeeze = o.hud_squeeze;
  d.hud_scale = o.hud_scale;
  d.major_mode_explicit = o.major_mode_explicit;
  d.object_slot = o.object_slot;
  d.hud_draw = fh1_native_executor_ && fh1_native_executor_->IsHudDraw();
  d.vertex_shader = o.vertex_shader;
  d.pixel_shader = o.pixel_shader;
  d.primitive_polygonal = o.primitive_polygonal;
  d.is_rasterization_done = o.is_rasterization_done;
  d.coverage_from_shader = draw_coverage_from_shader_;
  d.depth_control = o.depth_control;
  d.color_mask = o.color_mask;
  d.primitive = r;
  d.vertex_modification = o.vertex_modification;
  d.pixel_modification = o.pixel_modification;
  d.vertex_translation = o.vertex_translation;
  d.pixel_translation = o.pixel_translation;
  d.render_pass_key = o.render_pass_key;
  d.pipeline_handle = o.pipeline_handle;
  d.viewport = *o.viewport;
  d.index_version =
      r.index_buffer_type == IndexType::kGuestDMA ? o.index_version : UINT32_MAX;
  d.valid = true;
  ++s.recorded;
}

bool VulkanCommandProcessor::EnsureIndexArena() {
  if (index_arena_buffer_ != VK_NULL_HANDLE) return true;
  if (index_arena_failed_) return false;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          GetVulkanDevice(), kIndexArenaSize,
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, index_arena_buffer_,
          index_arena_memory_)) {
    REXGPU_WARN("Draw programs: failed to create the {} MB index version arena",
                kIndexArenaSize >> 20);
    index_arena_failed_ = true;
    return false;
  }
  index_arena_free_.clear();
  index_arena_free_.emplace(0, kIndexArenaSize);
  return true;
}

void VulkanCommandProcessor::IndexVersionWritten(
    const std::unique_lock<std::recursive_mutex>& global_lock, void* context, void* data,
    uint64_t argument, bool invalidated_by_gpu) {
  (void)global_lock;
  (void)context;
  (void)argument;
  (void)invalidated_by_gpu;
  // The shared memory cancelled the watch before calling: nothing to unwatch.
  static_cast<IndexVersion*>(data)->written.store(true, std::memory_order_release);
}

uint32_t VulkanCommandProcessor::AcquireIndexVersion(uint32_t guest_base, uint32_t bytes) {
  DrawProgramStats& s = draw_program_stats_;
  const uint64_t range = uint64_t(guest_base) << 32 | bytes;
  if (auto it = index_version_by_range_.find(range); it != index_version_by_range_.end()) {
    IndexVersion& version = index_versions_[it->second];
    if (version.alive && !version.written.load(std::memory_order_acquire)) return it->second;
    if (version.alive) RetireIndexVersion(it->second);
  }
  if (!bytes || !EnsureIndexArena()) return UINT32_MAX;
  const VkDeviceSize size =
      (VkDeviceSize(bytes) + kIndexVersionAlignment - 1) & ~(kIndexVersionAlignment - 1);
  auto slot = std::find_if(index_arena_free_.begin(), index_arena_free_.end(),
                           [size](const auto& range) { return range.second >= size; });
  if (slot == index_arena_free_.end()) {
    // Full: the draw reads the guest range as usual until space is freed.
    FreeRetiredIndexVersions();
    slot = std::find_if(index_arena_free_.begin(), index_arena_free_.end(),
                        [size](const auto& range) { return range.second >= size; });
    if (slot == index_arena_free_.end()) {
      ++s.arena_full;
      return UINT32_MAX;
    }
  }
  const VkDeviceSize offset = slot->first;
  const VkDeviceSize left = slot->second - size;
  index_arena_free_.erase(slot);
  if (left) index_arena_free_.emplace(offset + size, left);

  const uint32_t index = uint32_t(index_versions_.size());
  IndexVersion& version = index_versions_.emplace_back();
  version.guest_base = guest_base;
  version.bytes = bytes;
  version.offset = offset;
  // Watch first, then request: a write before the request is uploaded by
  // it, a write after it fires the watch.
  shared_memory_->WatchMemoryRange(guest_base, bytes, IndexVersionWritten, this, &version, index);
  {
    SharedMemory::UploadKindScope upload_kind(*shared_memory_, SharedMemory::UploadKind::kIndex);
    if (!shared_memory_->RequestRange(guest_base, bytes)) {
      // Left as a retired version (the watch may still fire into it).
      version.written.store(true, std::memory_order_relaxed);
      retired_index_versions_.push_back(
          {offset, size, GetCurrentSubmission(), GetCurrentSubmission(),
           std::chrono::steady_clock::now()});
      return UINT32_MAX;
    }
  }
  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);
  VkBufferCopy region = {};
  region.srcOffset = guest_base;
  region.dstOffset = offset;
  region.size = bytes;
  deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(), index_arena_buffer_, 1,
                                           &region);
  PushBufferMemoryBarrier(index_arena_buffer_, offset, size, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_INDEX_READ_BIT);
  version.last_submission = GetCurrentSubmission();
  version.alive = true;
  if (draw_program_mode_ == 1) {
    if (const void* guest = memory_->TranslatePhysical(guest_base)) {
      version.verify_hash = XXH3_64bits(guest, bytes);
    }
  }
  index_version_by_range_[range] = index;
  ++s.versions_made;
  s.version_bytes += bytes;
  return index;
}

void VulkanCommandProcessor::RetireIndexVersion(uint32_t index) {
  IndexVersion& version = index_versions_[index];
  if (!version.alive) return;
  version.alive = false;
  const uint64_t range = uint64_t(version.guest_base) << 32 | version.bytes;
  if (auto it = index_version_by_range_.find(range);
      it != index_version_by_range_.end() && it->second == index) {
    index_version_by_range_.erase(it);
  }
  const VkDeviceSize size =
      (VkDeviceSize(version.bytes) + kIndexVersionAlignment - 1) & ~(kIndexVersionAlignment - 1);
  retired_index_versions_.push_back(
      {version.offset, size, version.last_submission, GetCurrentSubmission(),
       std::chrono::steady_clock::now()});
  ++draw_program_stats_.versions_retired;
}

void VulkanCommandProcessor::FreeRetiredIndexVersions() {
  DrawProgramStats& s = draw_program_stats_;
  const uint64_t completed = GetCompletedSubmission();
  const auto now = std::chrono::steady_clock::now();
  // Retired in order, but a version may have been read last long before it
  // was retired: free every one whose last reader is done.
  for (auto it = retired_index_versions_.begin(); it != retired_index_versions_.end();) {
    if (it->last_submission > completed) {
      ++it;
      continue;
    }
    VkDeviceSize offset = it->offset, size = it->size;
    // Merge with the free neighbours.
    auto next = index_arena_free_.lower_bound(offset);
    if (next != index_arena_free_.end() && offset + size == next->first) {
      size += next->second;
      next = index_arena_free_.erase(next);
    }
    if (next != index_arena_free_.begin()) {
      auto previous = std::prev(next);
      if (previous->first + previous->second == offset) {
        offset = previous->first;
        size += previous->second;
        index_arena_free_.erase(previous);
      }
    }
    index_arena_free_.emplace(offset, size);
    const double ms = std::chrono::duration<double, std::milli>(now - it->retired).count();
    s.retire_ms += ms;
    s.retire_ms_max = std::max(s.retire_ms_max, ms);
    // Retired to freed, like the time above.
    s.retire_submissions += GetCurrentSubmission() - it->retired_submission;
    ++s.versions_freed;
    it = retired_index_versions_.erase(it);
  }
}

void VulkanCommandProcessor::DrawProgramFrame() {
  draw_program_mode_ = uint32_t(std::max(REXCVAR_GET(gpu_d3d_program), 0));
  draw_program_ = nullptr;
  FreeRetiredIndexVersions();
  if (++draw_program_swaps_ % kProgramSwaps) return;
  DrawProgramStats& s = draw_program_stats_;
  uint64_t evicted = 0;
  for (auto it = draw_programs_.begin(); it != draw_programs_.end();) {
    if (draw_program_swaps_ - it->second.last_swap > kProgramSwaps) {
      it = draw_programs_.erase(it);
      ++evicted;
      draw_program_cache_ = {};
    } else {
      ++it;
    }
  }
  if (!s.runs && !s.versions_freed) {
    s = {};
    return;
  }
  uint64_t live = 0;
  for (const IndexVersion& version : index_versions_) live += version.alive;
  const double frames = double(kProgramSwaps);
  const double draws = double(std::max<uint64_t>(s.draws, 1));
  REXGPU_INFO(
      "Draw programs (NA-2.4) mode {}: {:.1f} runs a frame, {} programs ({} made, {} evicted); "
      "{:.1f} draws a frame: from the program {:.1f} %, recorded {}, unrecordable {}, missed "
      "new {} state {} draw {} settings {} index {} targets {}; derived inside programs: "
      "analysis {} primitives {} translation {} pipeline {} viewport {}; shadow compared {}, "
      "mismatches {}, index mismatches {}; index versions made {} ({:.1f} KB copied), live {}, "
      "retired {}, freed {}, arena full {}, retire latency {:.1f} submissions, {:.2f} ms mean, "
      "{:.2f} ms max; timed (gpu_draw_cost_model) by mode as draws/draw ns/list entry ns: "
      "1 {}/{}/{}, 2 {}/{}/{}, 3 {}/{}/{}, 4 {}/{}/{}",
      draw_program_mode_, double(s.runs) / frames, draw_programs_.size(), s.programs_made,
      evicted, double(s.draws) / frames, 100.0 * double(s.hits) / draws, s.recorded,
      s.unrecordable, s.miss[kMissNew], s.miss[kMissState], s.miss[kMissDraw],
      s.miss[kMissSettings], s.miss[kMissIndex], s.miss[kMissTargets], s.derived_analysis,
      s.derived_primitives, s.derived_translation, s.derived_pipeline, s.derived_viewport,
      s.shadow_compared, s.shadow_mismatches, s.shadow_index_mismatches, s.versions_made,
      double(s.version_bytes) / 1024.0, live, s.versions_retired, s.versions_freed, s.arena_full,
      s.versions_freed ? double(s.retire_submissions) / double(s.versions_freed) : 0.0,
      s.versions_freed ? s.retire_ms / double(s.versions_freed) : 0.0, s.retire_ms_max,
      s.issue_draws[1], s.issue_ns[1], s.begin_ns[1], s.issue_draws[2], s.issue_ns[2],
      s.begin_ns[2], s.issue_draws[3], s.issue_ns[3], s.begin_ns[3], s.issue_draws[4],
      s.issue_ns[4], s.begin_ns[4]);
  const uint32_t logs = s.logs;
  s = {};
  s.logs = logs;
}

}  // namespace rex::graphics::vulkan
