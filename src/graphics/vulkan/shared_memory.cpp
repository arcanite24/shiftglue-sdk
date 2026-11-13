/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/fh1_native_executor.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/perf/counter.h>
#include <rex/ui/vulkan/util.h>

// Apple M1 (MoltenVK, Metal argument buffers): every pipeline barrier outside
// a render pass ends the Metal compute encoder, and the 21 ms free-roam drive
// frame was 9 ms of 15-20 us gaps between ~390 encoders a frame, ~90 of them
// single resolve dispatches split from their neighbours by this buffer's
// write-after-write barrier.
REXCVAR_DEFINE_BOOL(fh1_coalesce_resolve_writes, false, "GPU",
                    "Vulkan: skip the shared-memory barrier between consecutive compute "
                    "writes (resolves) to non-overlapping ranges; the next read's barrier "
                    "commits them all. Fewer encoder splits on MoltenVK")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_sparse_shared_memory, true, "GPU/Vulkan",
                    "Use sparse shared memory on Vulkan")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(vulkan_sparse_shared_memory_turnip, 1, "GPU/Vulkan",
                      "Sparse shared memory on Turnip: 0 never, 1 on the Adreno 740 only "
                      "(checked with Turnip gen8-v37, NN-6.1), 2 on every Adreno")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::vulkan {

VulkanSharedMemory::VulkanSharedMemory(VulkanCommandProcessor& command_processor,
                                       memory::Memory& memory,
                                       VkPipelineStageFlags guest_shader_pipeline_stages)
    : SharedMemory(memory),
      command_processor_(command_processor),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

VulkanSharedMemory::~VulkanSharedMemory() {
  Shutdown(true);
}

bool VulkanSharedMemory::Initialize() {
  InitializeCommon();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

  // Try to create a sparse buffer.
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = sparse_flags;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  // Turnip (Mesa, loaded on Android through android_gpu_driver) advertises
  // sparse residency. With an older Turnip, resolves written into the sparse
  // buffer read back as zeros on the Adreno 740, so the whole frame came out
  // black; with the bundled gen8-v37 the Odin 2 (Adreno 740) race renders as
  // with a dense buffer and its device memory falls by 290 MB (NN-6.1). Other
  // Adrenos have not been checked, so they keep the dense buffer by default.
  bool sparse_broken = false;
  if (vulkan_device->properties().driverID == VK_DRIVER_ID_MESA_TURNIP) {
    switch (REXCVAR_GET(vulkan_sparse_shared_memory_turnip)) {
      case 0:
        sparse_broken = true;
        break;
      case 1:
        sparse_broken = std::string_view(vulkan_device->properties().deviceName).find("740") ==
                        std::string_view::npos;
        break;
      default:
        break;
    }
  }
  if (sparse_broken && REXCVAR_GET(vulkan_sparse_shared_memory)) {
    REXGPU_INFO("Shared memory: not using sparse residency on Turnip");
  }
  if (REXCVAR_GET(vulkan_sparse_shared_memory) && !sparse_broken &&
      vulkan_device->properties().sparseResidencyBuffer) {
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) == VK_SUCCESS) {
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
      if (rex::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                    vulkan_device->memory_types().device_local,
                                &buffer_memory_type_)) {
        uint32_t allocation_size_log2;
        rex::bit_scan_forward(std::max(uint64_t(buffer_memory_requirements.alignment), uint64_t(1)),
                              &allocation_size_log2);
        if (allocation_size_log2 < kBufferSizeLog2) {
          // Maximum of 1024 allocations in the worst case for all of the
          // buffer because of the overall 4096 allocation count limit on
          // Windows drivers.
          InitializeSparseHostGpuMemory(std::max(
              allocation_size_log2,
              std::max(kHostGpuMemoryOptimalSparseAllocationLog2, kBufferSizeLog2 - uint32_t(10))));
        } else {
          // Shouldn't happen on any real platform, but no point allocating the
          // buffer sparsely.
          dfn.vkDestroyBuffer(device, buffer_, nullptr);
          buffer_ = VK_NULL_HANDLE;
        }
      } else {
        REXGPU_ERROR(
            "Shared memory: Failed to get a device-local Vulkan memory type "
            "for the sparse buffer");
        dfn.vkDestroyBuffer(device, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
      }
    } else {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan sparse buffer",
                   kBufferSize >> 20);
    }
  }

  // Create a non-sparse buffer if there were issues with the sparse buffer.
  if (buffer_ == VK_NULL_HANDLE) {
    REXGPU_INFO(
        "Vulkan sparse binding is not used for shared memory emulation - video "
        "memory usage may increase significantly because a full {} MB buffer "
        "will be created",
        kBufferSize >> 20);
    buffer_create_info.flags &= ~sparse_flags;
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) != VK_SUCCESS) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    VkMemoryRequirements buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
    if (!rex::bit_scan_forward(
            buffer_memory_requirements.memoryTypeBits & vulkan_device->memory_types().device_local,
            &buffer_memory_type_)) {
      REXGPU_ERROR(
          "Shared memory: Failed to get a device-local Vulkan memory type for "
          "the buffer");
      Shutdown();
      return false;
    }
    VkMemoryAllocateInfo buffer_memory_allocate_info;
    VkMemoryAllocateInfo* buffer_memory_allocate_info_last = &buffer_memory_allocate_info;
    buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_allocate_info.pNext = nullptr;
    buffer_memory_allocate_info.allocationSize = buffer_memory_requirements.size;
    buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
    VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_allocate_info;
    if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
      buffer_memory_allocate_info_last->pNext = &buffer_memory_dedicated_allocate_info;
      buffer_memory_allocate_info_last =
          reinterpret_cast<VkMemoryAllocateInfo*>(&buffer_memory_dedicated_allocate_info);
      buffer_memory_dedicated_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      buffer_memory_dedicated_allocate_info.pNext = nullptr;
      buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
      buffer_memory_dedicated_allocate_info.buffer = buffer_;
    }
    VkDeviceMemory buffer_memory;
    if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr, &buffer_memory) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
          "buffer",
          kBufferSize >> 20);
      Shutdown();
      return false;
    }
    buffer_memory_.push_back(buffer_memory);
    if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) != VK_SUCCESS) {
      REXGPU_ERROR("Shared memory: Failed to bind memory to the Vulkan buffer");
      Shutdown();
      return false;
    }
  }

  // The first usage will likely be uploading.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      rex::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize, size_t(1)
                                                                           << page_size_log2()));

  return true;
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::EndSubmission() {
  upload_buffer_pool_->FlushWrites();
}

void VulkanSharedMemory::Use(Usage usage, std::pair<uint32_t, uint32_t> written_range) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second = std::min(written_range.second, kBufferSize - written_range.first);
  assert_true(usage != Usage::kRead || !written_range.second);
  // Consecutive compute writes (FH1's resolves, ~90 a drive frame in bursts)
  // to ranges that do not overlap need no barrier between them: nothing reads
  // the earlier write before the next read usage, whose barrier commits every
  // write since the last one. On MoltenVK with Metal argument buffers each
  // pipeline barrier outside a render pass ends the compute encoder, so the
  // write-after-write barrier here cost a 15-20 us encoder gap per resolve;
  // the pending range grows to cover all the uncommitted writes.
  if (REXCVAR_GET(fh1_coalesce_resolve_writes) && usage == Usage::kComputeWrite &&
      last_usage_ == usage && last_written_range_.second && written_range.second &&
      (written_range.first >= last_written_range_.first + last_written_range_.second ||
       written_range.first + written_range.second <= last_written_range_.first)) {
    const uint32_t start = std::min(last_written_range_.first, written_range.first);
    const uint32_t end =
        std::max(last_written_range_.first + last_written_range_.second,
                 written_range.first + written_range.second);
    last_written_range_ = std::make_pair(start, end - start);
    return;
  }
  if (last_usage_ != usage || last_written_range_.second) {
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    GetUsageMasks(last_usage_, src_stage_mask, src_access_mask);
    GetUsageMasks(usage, dst_stage_mask, dst_access_mask);
    VkDeviceSize offset, size;
    if (last_usage_ == usage) {
      // Committing the previous write, while not changing the access mask
      // (passing false as whether to skip the barrier if no masks are changed
      // for this reason).
      offset = VkDeviceSize(last_written_range_.first);
      size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      offset = 0;
      size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    command_processor_.PushBufferMemoryBarrier(
        buffer_, offset, size, src_stage_mask, dst_stage_mask, src_access_mask, dst_access_mask,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  }
  last_written_range_ = written_range;
}

bool VulkanSharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                          uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = length_allocations
                                        << host_gpu_memory_sparse_granularity_log2();
  memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    REXGPU_ERROR("Shared memory: Failed to allocate sparse buffer memory");
    return false;
  }
  buffer_memory_.push_back(memory);

  VkSparseMemoryBind bind;
  bind.resourceOffset = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  bind.size = memory_allocate_info.allocationSize;
  bind.memory = memory;
  bind.memoryOffset = 0;
  bind.flags = 0;
  VkPipelineStageFlags bind_wait_stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (vulkan_device->properties().tessellationShader) {
    bind_wait_stage_mask |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  command_processor_.SparseBindBuffer(buffer_, 1, &bind, bind_wait_stage_mask);

  return true;
}

bool VulkanSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_page_ranges) {
    PERF_counter_add(kGpuUploadBytes, int64_t(upload_range.second) << page_size_log2());
  }
  // An upload while a rendering is open ends it: count it by what it was for.
  if (command_processor_.in_render_pass()) {
    switch (upload_kind()) {
      case UploadKind::kIndex:
        PERF_counter_inc(kGpuUploadSplitsIndex);
        break;
      case UploadKind::kTexture:
        PERF_counter_inc(kGpuUploadSplitsTexture);
        break;
      default:
        PERF_counter_inc(kGpuUploadSplitsVertex);
        break;
    }
  }
  // upload_page_ranges are sorted, use them to determine the range for the
  // ordering barrier.
  // A draw's upload found while its rendering is open goes behind the
  // rendering's begin instead of ending it (see BeginFh1HoistedUpload). The
  // usage barriers are recorded with the copy: the rendering read the buffer
  // before and reads it after, so the barrier state is the same on both sides.
  // Only a draw's reads are reordered: a write usage before the upload keeps
  // the guest order.
  // The uploads hoisted ahead of one rendering share one barrier pair: the
  // first records the barrier into the transfer (the whole buffer) before its
  // copies and the one back to the read after them; a later one, with the
  // buffer in the read state the group left, records only its copies, which
  // go between the group's copies and its barrier back. Its pages must not
  // repeat the group's: copies to the same range are unordered without a
  // barrier between them.
  const bool read_state = last_usage_ == Usage::kRead && !last_written_range_.second;
  bool merge = read_state && command_processor_.IsFh1HoistGroupOpen();
  if (merge) {
    for (const auto& range : upload_page_ranges) {
      for (const auto& group_range : hoist_group_page_ranges_) {
        if (range.first < group_range.first + group_range.second &&
            group_range.first < range.first + range.second) {
          merge = false;
          break;
        }
      }
      if (!merge) break;
    }
  }
  const VulkanCommandProcessor::Fh1HoistedUpload hoisted =
      (last_usage_ == Usage::kRead || last_usage_ == Usage::kTransferDestination)
          ? command_processor_.BeginFh1HoistedUpload(merge)
          : VulkanCommandProcessor::Fh1HoistedUpload::kNone;
  const std::pair<uint32_t, uint32_t> written_range(
      upload_page_ranges.front().first << page_size_log2(),
      (upload_page_ranges.back().first + upload_page_ranges.back().second -
       upload_page_ranges.front().first)
          << page_size_log2());
  if (hoisted == VulkanCommandProcessor::Fh1HoistedUpload::kMerged) {
    // Copies only, into the open group; the usage stays the read the group
    // left. If the rendering ends meanwhile the copies stay after its end,
    // outside the group, and get the barriers of their own they then need.
    hoist_group_page_ranges_.insert(hoist_group_page_ranges_.end(), upload_page_ranges.begin(),
                                    upload_page_ranges.end());
    const bool successful = UploadRangesCopies(upload_page_ranges);
    if (!command_processor_.EndFh1HoistedUpload()) {
      Use(Usage::kTransferDestination, written_range);
      command_processor_.RecordFh1CancelledHoistBarriers();
      Use(Usage::kRead);
    }
    return successful;
  }
  Use(Usage::kTransferDestination, written_range);
  if (hoisted == VulkanCommandProcessor::Fh1HoistedUpload::kGroup) {
    // The barrier into the transfer goes before the copies, the one back to
    // the reading usage after them, then the whole upload moves ahead of the
    // rendering's begin, so its draws see the data.
    hoist_group_page_ranges_.assign(upload_page_ranges.begin(), upload_page_ranges.end());
    command_processor_.RecordFh1HoistedBarriers();
    struct EndHoist {
      VulkanSharedMemory& memory;
      ~EndHoist() {
        memory.Use(Usage::kRead);
        memory.command_processor_.EndFh1HoistedUpload();
      }
    } end_hoist{*this};
    return UploadRangesCopies(upload_page_ranges);
  }
  command_processor_.SubmitBarriers(true);
  return UploadRangesCopies(upload_page_ranges);
}

bool VulkanSharedMemory::UploadRangesCopies(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  Fh1NativeExecutor* const gpu_profile_executor = command_processor_.GetFh1NativeExecutor();
  const uint64_t gpu_timing =
      gpu_profile_executor ? gpu_profile_executor->BeginLabelGpuTiming("upload") : UINT64_MAX;
  struct EndTiming {
    Fh1NativeExecutor* executor;
    uint64_t token;
    ~EndTiming() {
      if (executor) executor->EndLabelGpuTiming(token);
    }
  } end_timing{gpu_profile_executor, gpu_timing};
  command_processor_.BeginDebugLabel("shared memory upload");
  struct EndLabel {
    VulkanCommandProcessor& processor;
    ~EndLabel() { processor.EndDebugLabel(); }
  } end_label{command_processor_};
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset, upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      if (upload_buffer_previous != upload_buffer && !upload_regions_.empty()) {
        assert_true(upload_buffer_previous != VK_NULL_HANDLE);
        command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                       uint32_t(upload_regions_.size()), upload_regions_.data());
        upload_regions_.clear();
      }
      upload_buffer_previous = upload_buffer;
      VkBufferCopy& upload_region = upload_regions_.emplace_back();
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset = VkDeviceSize(upload_range_start << page_size_log2());
      upload_region.size = upload_buffer_size;
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    assert_true(upload_buffer_previous != VK_NULL_HANDLE);
    command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                   uint32_t(upload_regions_.size()), upload_regions_.data());
    upload_regions_.clear();
  }
  return successful;
}

void VulkanSharedMemory::GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      // Resolves write the buffer: the write must be made available to the
      // next usage (tiled GPUs do not tolerate a read-only source mask).
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
    default:
      break;
  }
  stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

}  // namespace rex::graphics::vulkan
