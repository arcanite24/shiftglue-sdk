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
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/thread/sync_census.h>

REXCVAR_DEFINE_BOOL(fh1_narrow_cpu_invalidation, false, "GPU/D3D12",
                    "Limit speculative CPU invalidation to 64 KiB windows")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics {

// critical_region_census (NN-3.10): this file's global lock acquisitions.
constexpr auto kSharedMemoryLockSource = rex::thread::LockSource::kSharedMemory;

namespace {
// Bits of 64-page block `block` that fall in pages [first, last].
uint64_t PageBlockMask(uint32_t block, uint32_t first, uint32_t last) {
  if (first > last || (last >> 6) < block || (first >> 6) > block) {
    return 0;
  }
  uint64_t mask = UINT64_MAX;
  if ((first >> 6) == block) {
    mask &= ~((uint64_t(1) << (first & 63)) - 1);
  }
  if ((last >> 6) == block && (last & 63) != 63) {
    mask &= (uint64_t(1) << ((last & 63) + 1)) - 1;
  }
  return mask;
}
}  // namespace

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  // Guest pages, not host pages. With 16 KB host pages (Apple silicon) a
  // 4 KB resolve shares its tracking page with unrelated CPU data, and a CPU
  // write there invalidated the resolve, so draws read stale memory instead
  // (auto exposure from an old luminance resolve: an overbright, flashing
  // scene). Host protection still works on whole host pages; see
  // MemoryInvalidationCallback for how the excess is handled.
  page_size_log2_ = 12;
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  block_invalidation_epochs_ = std::vector<std::atomic<uint32_t>>(num_system_page_flags_);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  delete[] page_upload_kind_.exchange(nullptr, std::memory_order_acq_rel);

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
  block_invalidation_epochs_ = std::vector<std::atomic<uint32_t>>();
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);

  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));
  std::fill(system_page_flags_valid_and_gpu_written_.begin(),
            system_page_flags_valid_and_gpu_written_.end(), uint64_t(0));
  const uint32_t epoch = invalidation_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  for (std::atomic<uint32_t>& block : block_invalidation_epochs_) {
    block.store(epoch, std::memory_order_release);
  }
}

bool SharedMemory::InvalidatedSince(uint32_t page_first, uint32_t page_last,
                                    uint32_t epoch) const {
  for (uint32_t i = page_first >> 6; i <= (page_last >> 6) && i < num_system_page_flags_; ++i) {
    if (int32_t(block_invalidation_epochs_[i].load(std::memory_order_acquire) - epoch) > 0) {
      return true;
    }
  }
  return false;
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);

  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory.
  system_page_flags_valid_ = system_page_flags_valid_and_gpu_written_;
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      if (page_first > range->page_last || page_last < range->page_first) {
        node = node->bucket_node_next;
        continue;
      }

      // Retire a one-shot watch before invoking it. The global critical region
      // is recursive, and a callback may indirectly invalidate shared memory
      // again. Leaving the watch linked during the callback would let nested
      // FireWatches calls dispatch the same watch recursively until the host
      // thread exhausts its stack.
      WatchCallback callback = range->callback;
      void* callback_context = range->callback_context;
      void* callback_data = range->callback_data;
      uint64_t callback_argument = range->callback_argument;
      UnlinkWatchRange(range);
      callback(global_lock, callback_context, callback_data, callback_argument,
               invalidated_by_gpu);

      // UnlinkWatchRange recycles every node owned by the range, and a nested
      // callback may recycle other nodes too. Restart from the live bucket head
      // rather than following a potentially recycled pointer.
      node = watch_buckets_[i];
    }
  }
}

bool SharedMemory::CopyCpuRange(uint32_t start, std::span<uint8_t> destination) {
  if (start > kBufferSize || destination.size() > kBufferSize - start) {
    return false;
  }
  if (destination.empty()) {
    return true;
  }
  if (!memory_invalidation_callback_handle_) return false;
  // Protect before reading, outside the global lock (memory heaps have their own locks).
  {
    rex::thread::census::ProtectOwnerScope owner(rex::thread::census::ProtectOwner::kArmCopyCpu);
    memory().EnablePhysicalMemoryAccessCallbacks(start, uint32_t(destination.size()), true,
                                                 false);
  }
  const uint32_t first = start >> page_size_log2_;
  const uint32_t last = (start + uint32_t(destination.size()) - 1) >> page_size_log2_;
  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);
  if ((last >> 6) >= system_page_flags_valid_and_gpu_written_.size()) {
    return false;
  }
  for (uint32_t block = first >> 6; block <= last >> 6; ++block) {
    uint64_t mask = UINT64_MAX;
    if (block == first >> 6) mask &= UINT64_MAX << (first & 63);
    if (block == last >> 6 && (last & 63) != 63) mask &= (uint64_t(1) << ((last & 63) + 1)) - 1;
    if (system_page_flags_valid_and_gpu_written_[block] & mask) {
      return false;
    }
  }
  std::memcpy(destination.data(), memory().TranslatePhysical(start), destination.size());
  return true;
}

bool SharedMemory::CopyCpuSnapshot(uint32_t start, std::span<uint8_t> destination) {
  if (start > kBufferSize || destination.size() > kBufferSize - start) return false;
  if (destination.empty()) return true;
  WatchHandle watch = nullptr;
  {
    auto lock = global_critical_region_.Acquire(kSharedMemoryLockSource);
    watch = WatchMemoryRange(start, uint32_t(destination.size()),
        [](const auto&, void*, void* data, uint64_t, bool) {
          *static_cast<WatchHandle*>(data) = nullptr;
        }, nullptr, &watch, 0);
    if (!watch) return false;
  }
  // CopyCpuRange takes heap locks before the global lock. Do not retain the
  // global lock across it, even though the critical region is recursive.
  const bool copied = CopyCpuRange(start, destination);
  auto lock = global_critical_region_.Acquire(kSharedMemoryLockSource);
  const bool unchanged = watch != nullptr;
  if (watch) UnwatchMemoryRange(watch);
  return copied && unchanged;
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

void SharedMemory::CopyReadbackToGuest(uint32_t start, const uint8_t* source, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_size = uint32_t(1) << page_size_log2_;
  for (uint32_t offset = 0; offset < length;) {
    const uint32_t address = start + offset;
    const uint32_t chunk = std::min(length - offset, page_size - (address & (page_size - 1)));
    const uint32_t page = address >> page_size_log2_;
    bool valid;
    {
      auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);
      valid = (system_page_flags_valid_[page >> 6] >> (page & 63)) & 1;
    }
    // Not under the lock: the store may fault on the page's write watch.
    if (valid) {
      if (uint8_t* destination = memory().TranslatePhysical(address)) {
        std::memcpy(destination, source + offset, chunk);
      }
    }
    offset += chunk;
  }
}

std::pair<uint32_t, uint32_t> SharedMemory::CountValidPages(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) return {0, 0};
  length = std::min(length, kBufferSize - start);
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);
  uint32_t valid = 0, gpu_written = 0;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    const uint64_t bit = uint64_t(1) << (page & 63);
    valid += (system_page_flags_valid_[page >> 6] & bit) != 0;
    gpu_written += (system_page_flags_valid_and_gpu_written_[page >> 6] & bit) != 0;
  }
  return {valid, gpu_written};
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      system_page_flags_valid_[i] |= valid_bits;
      uint64_t& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~valid_bits);
    }
  }

  if (rex::thread::census::WriteWatchEnabled()) {
    NotePageUploadKind(valid_page_first, valid_page_last, written_by_gpu);
  }

  if (memory_invalidation_callback_handle_) {
    using rex::thread::census::ProtectOwner;
    ProtectOwner owner = ProtectOwner::kArmUploadOther;
    if (written_by_gpu) {
      owner = ProtectOwner::kArmGpuWritten;
    } else {
      switch (upload_kind_) {
        case UploadKind::kVertex: owner = ProtectOwner::kArmVertex; break;
        case UploadKind::kIndex: owner = ProtectOwner::kArmIndex; break;
        case UploadKind::kTexture: owner = ProtectOwner::kArmTexture; break;
        case UploadKind::kMemexport: owner = ProtectOwner::kArmMemexport; break;
        default: break;
      }
    }
    rex::thread::census::ProtectOwnerScope owner_scope(owner);
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

void SharedMemory::NotePageUploadKind(uint32_t page_first, uint32_t page_last,
                                      bool written_by_gpu) {
  std::atomic<uint8_t>* kinds = page_upload_kind_.load(std::memory_order_acquire);
  if (!kinds) {
    const size_t count = size_t(kBufferSize) >> page_size_log2_;
    auto* fresh = new std::atomic<uint8_t>[count];
    for (size_t i = 0; i < count; ++i) {
      fresh[i].store(rex::thread::census::kPageKindNone, std::memory_order_relaxed);
    }
    if (page_upload_kind_.compare_exchange_strong(kinds, fresh, std::memory_order_acq_rel)) {
      kinds = fresh;
    } else {
      delete[] fresh;
    }
  }
  const uint8_t kind =
      written_by_gpu ? rex::thread::census::kPageKindGpuWritten : uint8_t(upload_kind_);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    kinds[page].store(kind, std::memory_order_relaxed);
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  // Reused across calls: this runs several times per draw.
  std::vector<std::pair<uint32_t, uint32_t>>& merged_ranges = request_ranges_scratch_;
  merged_ranges.clear();
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    // No lock: invalidations clear valid bits from guest threads under the
    // global critical region, and one landing after this read is the same as
    // one landing right after a locked scan (the draw would use the data
    // either way; the next request uploads it). Uploads mark pages valid
    // under the lock, so no invalidation during an upload is lost. The lock
    // is the process-wide one guest events take, so skipping it here takes
    // the GPU commands thread out of their contention on every draw.
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid =
            reinterpret_cast<const volatile uint64_t&>(system_page_flags_valid_[i]);
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  if (upload_ranges_.empty()) {
    return true;
  }

  ++upload_request_count_;
  UploadStats& stats = upload_stats_[size_t(upload_kind_)];
  ++stats.requests;
  stats.ranges += upload_ranges_.size();
  if (page_upload_frame_.empty()) {
    page_upload_frame_.resize(size_t(kBufferSize) >> page_size_log2_);
  }
  for (const auto& range : upload_ranges_) {
    stats.bytes += uint64_t(range.second) << page_size_log2_;
    for (uint32_t page = range.first; page < range.first + range.second; ++page) {
      if (page_upload_frame_[page] == upload_frame_) {
        stats.repeat_bytes += uint64_t(1) << page_size_log2_;
      }
      page_upload_frame_[page] = upload_frame_;
    }
  }
  return UploadRanges(upload_ranges_);
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  // Nearly every request (vertex and index data, per draw) finds its pages
  // valid: answer that from the bitmaps, without the merge and upload scan.
  if (length && start <= kBufferSize && kBufferSize - start >= length) {
    const uint32_t page_first = start >> page_size_log2_;
    const uint32_t page_last = (start + length - 1) >> page_size_log2_;
    // Same reasoning as the scan in RequestRanges for reading without the lock.
    bool valid = true;
    for (uint32_t i = page_first >> 6; valid && i <= page_last >> 6; ++i) {
      uint64_t block_valid =
          reinterpret_cast<const volatile uint64_t&>(system_page_flags_valid_[i]);
      if (i == page_first >> 6) {
        block_valid |= (uint64_t(1) << (page_first & 63)) - 1;
      }
      if (i == page_last >> 6 && (page_last & 63) != 63) {
        block_valid |= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      }
      valid = block_valid == ~uint64_t(0);
    }
    if (valid && host_gpu_memory_sparse_granularity_log2_ != UINT32_MAX) {
      const uint32_t allocation_first = start >> host_gpu_memory_sparse_granularity_log2_;
      const uint32_t allocation_last =
          (start + length - 1) >> host_gpu_memory_sparse_granularity_log2_;
      for (uint32_t a = allocation_first; valid && a <= allocation_last; ++a) {
        valid = (host_gpu_memory_sparse_allocated_[a >> 6] >> (a & 63)) & 1;
      }
    }
    if (valid) {
      return true;
    }
  }
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire(kSharedMemoryLockSource);

  if (rex::thread::census::WriteWatchEnabled()) {
    // A fault on this thread: what made the faulting page valid (NN-3.12).
    const std::atomic<uint8_t>* kinds = page_upload_kind_.load(std::memory_order_acquire);
    rex::thread::census::NoteFaultPage(
        kinds ? kinds[page_first].load(std::memory_order_relaxed)
              : rex::thread::census::kPageKindNone,
        (system_page_flags_valid_[block_first] >> (page_first & 63)) & 1);
  }

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
    if (REXCVAR_GET(fh1_narrow_cpu_invalidation)) {
      // Keep the actual write and existing GPU-history guards. Only reduce the
      // speculative excess, so neighboring geometry windows stay protected.
      const uint32_t mask = (uint32_t(1) << (16 - std::min(page_size_log2_, uint32_t(16)))) - 1;
      page_first = std::max(page_first, (physical_address_start >> page_size_log2_) & ~mask);
      page_last = std::min(page_last, (physical_address_last >> page_size_log2_) | mask);
    }
  }

  // The range is rounded out to host pages. When those are larger than the
  // tracking pages, keep GPU-written pages the write didn't touch: they are
  // not in sync with CPU memory, so reuploading them would lose the GPU data.
  uint32_t keep_page_first = 1, keep_page_last = 0;
  uint32_t exact_page_first = 1, exact_page_last = 0;
  if (page_size_log2_ < rex::log2_ceil(uint32_t(rex::memory::page_size()))) {
    auto exact = memory::CurrentInvalidationExactPhysicalRange();
    if (exact.second && exact.first >= physical_address_start &&
        exact.first - physical_address_start < length) {
      keep_page_first = physical_address_start >> page_size_log2_;
      keep_page_last = physical_address_last >> page_size_log2_;
      exact_page_first = exact.first >> page_size_log2_;
      exact_page_last =
          std::min(exact.first + (exact.second - 1), physical_address_last) >> page_size_log2_;
    }
  }

  bool any_kept = false;
  const uint32_t invalidation_epoch =
      invalidation_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  for (uint32_t i = block_first; i <= block_last; ++i) {
    block_invalidation_epochs_[i].store(invalidation_epoch, std::memory_order_release);
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (keep_page_first <= keep_page_last) {
      uint64_t keep_bits = system_page_flags_valid_and_gpu_written_[i] &
                           PageBlockMask(i, keep_page_first, keep_page_last) &
                           ~PageBlockMask(i, exact_page_first, exact_page_last);
      invalidate_bits &= ~keep_bits;
      any_kept |= keep_bits != 0;
    }
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
  }

  if (!any_kept) {
    FireWatches(page_first, page_last, false);
  } else {
    // Only the invalidated runs: a watch on a kept page would drop textures
    // that still match it.
    uint32_t run_first = page_first;
    for (uint32_t page = page_first; page <= page_last + 1; ++page) {
      bool kept = page <= page_last &&
                  ((system_page_flags_valid_and_gpu_written_[page >> 6] >> (page & 63)) & 1);
      if (page > page_last || kept) {
        if (run_first < page) {
          FireWatches(run_first, page - 1, false);
        }
        run_first = page + 1;
      }
    }
  }

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
