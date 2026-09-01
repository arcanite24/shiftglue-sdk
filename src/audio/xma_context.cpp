/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2021 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*
* @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
*/

#include <algorithm>
#include <cstring>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/helpers.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/perf/counter.h>
#include <rex/platform.h>
#include <rex/stream.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

REXCVAR_DEFINE_BOOL(
    xma_relaxed_padding_admission, false, "Audio",
    "Treat XMA output padding as best-effort headroom instead of a hard decode requirement");

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

namespace rex::audio {

using stream::BitStream;

const uint32_t XmaContext::kBitsPerPacketHeader;
const uint32_t XmaContext::kOutputMaxSizeBytes;

bool XmaStallTracker::ShouldLogSummary(uint64_t total_count) {
  return total_count == 1 || total_count == 8 || total_count == 64 ||
         (total_count >= 256 && total_count % 256 == 0);
}

bool XmaNoSpaceObservation::Matches(const XmaNoSpaceObservation& other) const {
  return input_offset == other.input_offset && remaining_blocks == other.remaining_blocks &&
         required_blocks == other.required_blocks && current_buffer == other.current_buffer &&
         output_read_offset == other.output_read_offset &&
         output_write_offset == other.output_write_offset &&
         input_buffer_0_valid == other.input_buffer_0_valid &&
         input_buffer_1_valid == other.input_buffer_1_valid;
}

XmaNoSpaceObservationResult XmaStallTracker::ObserveNoSpace(
    const XmaNoSpaceObservation& observation) {
  if (has_no_space_observation_ && no_space_observation_.Matches(observation)) {
    ++no_space_observation_count_;
    return {
        .repeated = no_space_observation_count_ >= kNoSpaceConfirmationObservations,
        .log_recovery = false,
    };
  }

  const bool log_recovery = NoteProgress(observation.input_offset, observation.output_read_offset,
                                         observation.output_write_offset);
  no_space_observation_ = observation;
  no_space_observation_count_ = 1;
  has_no_space_observation_ = true;
  return {.repeated = false, .log_recovery = log_recovery};
}

bool XmaStallTracker::NoteNoSpaceStall() {
  ++metrics_.consecutive_no_space_stalls;
  ++metrics_.total_no_space_stalls;
  const bool should_log = ShouldLogSummary(metrics_.total_no_space_stalls);
  recovery_log_pending_ |= should_log;
  return should_log;
}

bool XmaStallTracker::NoteNoProgressStall() {
  ++metrics_.consecutive_no_progress_stalls;
  ++metrics_.total_no_progress_stalls;
  const bool should_log = ShouldLogSummary(metrics_.total_no_progress_stalls);
  recovery_log_pending_ |= should_log;
  return should_log;
}

bool XmaStallTracker::NoteProgress(uint32_t input_offset, uint8_t output_read_offset,
                                   uint8_t output_write_offset) {
  const bool recovered =
      metrics_.consecutive_no_space_stalls != 0 || metrics_.consecutive_no_progress_stalls != 0;
  if (recovered) {
    ++metrics_.total_recoveries;
  }
  const bool should_log_recovery = recovered && recovery_log_pending_;
  metrics_.consecutive_no_space_stalls = 0;
  metrics_.consecutive_no_progress_stalls = 0;
  no_space_observation_count_ = 0;
  has_no_space_observation_ = false;
  recovery_log_pending_ = false;
  metrics_.last_progress_input_offset = input_offset;
  metrics_.last_progress_output_read_offset = output_read_offset;
  metrics_.last_progress_output_write_offset = output_write_offset;
  return should_log_recovery;
}

void XmaStallTracker::Reset(uint32_t initial_input_offset) {
  metrics_ = {};
  no_space_observation_count_ = 0;
  has_no_space_observation_ = false;
  recovery_log_pending_ = false;
  metrics_.last_progress_input_offset = initial_input_offset;
}

XmaPacketHandle ResolvePacket(const XMA_CONTEXT_DATA& data, uint32_t starting_buffer_index,
                              uint32_t logical_packet_index, uint32_t current_buffer_packet_count) {
  XmaPacketHandle result;
  if (starting_buffer_index > 1 ||
      current_buffer_packet_count !=
          data.GetInputBufferPacketCount(static_cast<uint8_t>(starting_buffer_index))) {
    return result;
  }

  result.buffer_index = starting_buffer_index;
  result.packet_index = logical_packet_index;
  if (logical_packet_index >= current_buffer_packet_count) {
    result.buffer_index ^= 1;
    result.packet_index -= current_buffer_packet_count;
  }

  const auto buffer_index = static_cast<uint8_t>(result.buffer_index);
  if (!data.IsInputBufferValid(buffer_index)) {
    result.status = XmaPacketStatus::kBufferInvalid;
    return result;
  }
  if (!data.GetInputBufferAddress(buffer_index)) {
    result.status = XmaPacketStatus::kNullAddress;
    return result;
  }
  if (result.packet_index >= data.GetInputBufferPacketCount(buffer_index)) {
    result.status = XmaPacketStatus::kPacketOutOfRange;
    return result;
  }

  result.status = XmaPacketStatus::kValid;
  return result;
}

bool XmaContext::IsValidFrameSize(uint32_t frame_size) {
  return frame_size != 0 && frame_size != xma::kMaxFrameLength;
}

XmaContext::XmaContext() : work_completion_event_(rex::thread::Event::CreateAutoResetEvent(false)) {
  ResetStallMetrics();
}

XmaContext::~XmaContext() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContext::Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REXAPU_ERROR("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

bool XmaContext::Work() {
  if (!is_allocated() || !is_enabled()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  // Consume-only context: no input, just drain remaining subframes.
  if (data.IsConsumeOnlyContext()) {
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    Consume(&output_rb, &data);
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  // Minimum free blocks needed before attempting a decode.
  // Use subframe_decode_count (clamped to 1) instead of full frame size.
  const uint32_t effective_sdc = std::max(static_cast<uint32_t>(1), data.subframe_decode_count);
  const bool relaxed_padding = REXCVAR_GET(xma_relaxed_padding_admission);
  const int32_t minimum_subframe_decode_count =
      static_cast<int32_t>(effective_sdc) + (relaxed_padding ? 0 : data.output_buffer_padding);

  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    NoteNoSpaceStall(data, minimum_subframe_decode_count);
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  while (remaining_subframe_blocks_in_output_buffer_ >= minimum_subframe_decode_count) {
    const uint32_t previous_input_offset = data.input_buffer_read_offset;
    const uint8_t previous_output_write_offset =
        static_cast<uint8_t>(output_rb.write_offset() / kOutputBytesPerBlock);
    const uint8_t previous_current_buffer = data.current_buffer;
    const bool previous_buffer_0_valid = data.input_buffer_0_valid != 0;
    const bool previous_buffer_1_valid = data.input_buffer_1_valid != 0;
    const uint8_t previous_remaining_subframes = current_frame_remaining_subframes_;

    Decode(&data);
    Consume(&output_rb, &data);

    const uint8_t current_output_write_offset =
        static_cast<uint8_t>(output_rb.write_offset() / kOutputBytesPerBlock);
    const bool made_progress = data.input_buffer_read_offset != previous_input_offset ||
                               current_output_write_offset != previous_output_write_offset ||
                               current_frame_remaining_subframes_ != previous_remaining_subframes ||
                               data.current_buffer != previous_current_buffer ||
                               (data.input_buffer_0_valid != 0) != previous_buffer_0_valid ||
                               (data.input_buffer_1_valid != 0) != previous_buffer_1_valid;
    if (made_progress) {
      NoteProgress(data, output_rb, previous_input_offset);
    } else {
      NoteNoProgressStall(data);
      break;
    }

    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {
      break;
    }
  }

  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  if (output_rb.empty()) {
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

void XmaContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
}

bool XmaContext::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContext::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_NOISY_DEBUG("XmaContext: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XmaContext::ClearLocked(XMA_CONTEXT_DATA* data) {
  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  ResetDecoderState();
}

void XmaContext::ResetDecoderState() {
  // A freed or re-initialized context is a new logical stream, so the previous
  // wave's MDCT overlap-add tail must not survive into frame 0 of the next one.
  // avcodec_flush_buffers() cannot drop it: ff_xmaframes_decoder declares no
  // flush callback, so the call never reaches the code clearing channel[].out.
  // Invalidating the cached format makes PrepareDecoder reopen the codec on the
  // next decode, which does discard the history.
  if (av_context_) {
    av_context_->sample_rate = 0;
    av_context_->channels = 0;
  }
  raw_frame_.fill(0);
  decoded_frame_.fill(0);
  carry_frame_.fill(0);
  carry_valid_ = false;
  pending_output_limit_ = 0;
  pending_start_skip_ = 0;
  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
  packet_warning_mask_ = 0;
  payload_warning_mask_ = 0;
  ResetStallMetrics();
}

void XmaContext::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);
  ResetStallMetrics();
}

void XmaContext::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  ResetDecoderState();
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
}

void XmaContext::NoteNoSpaceStall(const XMA_CONTEXT_DATA& data,
                                  int32_t minimum_subframe_decode_count) {
  const auto previous = stall_tracker_.metrics();
  const XmaNoSpaceObservation observation = {
      .input_offset = data.input_buffer_read_offset,
      .remaining_blocks = remaining_subframe_blocks_in_output_buffer_,
      .required_blocks = minimum_subframe_decode_count,
      .current_buffer = data.current_buffer,
      .output_read_offset = data.output_buffer_read_offset,
      .output_write_offset = data.output_buffer_write_offset,
      .input_buffer_0_valid = data.input_buffer_0_valid != 0,
      .input_buffer_1_valid = data.input_buffer_1_valid != 0,
  };
  const auto observation_result = stall_tracker_.ObserveNoSpace(observation);
  if (observation_result.log_recovery) {
    PROFILE_XMA_STALL_RECOVERY();
    REXAPU_WARN(
        "XmaContext {}: recovered after stalls (no-space {}, no-progress "
        "{}): buffer {}, input {}, output {}/{}",
        id(), previous.consecutive_no_space_stalls, previous.consecutive_no_progress_stalls,
        data.current_buffer, data.input_buffer_read_offset, data.output_buffer_read_offset,
        data.output_buffer_write_offset);
  }
  if (!observation_result.repeated) {
    return;
  }

  PROFILE_XMA_NO_SPACE_STALL();
  if (!stall_tracker_.NoteNoSpaceStall()) {
    return;
  }
  const auto& metrics = stall_tracker_.metrics();
  REXAPU_WARN(
      "XmaContext {}: output-space stall x{} (total {}): need {} blocks, "
      "have {}, buffer {}, input {}, output {}/{}, sdc {}, padding {}, "
      "relaxed {}, last progress input {} output {}/{}",
      id(), metrics.consecutive_no_space_stalls, metrics.total_no_space_stalls,
      minimum_subframe_decode_count, remaining_subframe_blocks_in_output_buffer_,
      data.current_buffer, data.input_buffer_read_offset, data.output_buffer_read_offset,
      data.output_buffer_write_offset, data.subframe_decode_count, data.output_buffer_padding,
      REXCVAR_GET(xma_relaxed_padding_admission) ? 1 : 0, metrics.last_progress_input_offset,
      metrics.last_progress_output_read_offset, metrics.last_progress_output_write_offset);
}

void XmaContext::NoteNoProgressStall(const XMA_CONTEXT_DATA& data) {
  PROFILE_XMA_NO_PROGRESS_STALL();
  if (!stall_tracker_.NoteNoProgressStall()) {
    return;
  }
  const auto& metrics = stall_tracker_.metrics();
  REXAPU_WARN(
      "XmaContext {}: no-progress stall x{} (total {}): buffer {}, input {}, "
      "output {}/{}, error {}, last progress input {} output {}/{}",
      id(), metrics.consecutive_no_progress_stalls, metrics.total_no_progress_stalls,
      data.current_buffer, data.input_buffer_read_offset, data.output_buffer_read_offset,
      data.output_buffer_write_offset, data.error_status, metrics.last_progress_input_offset,
      metrics.last_progress_output_read_offset, metrics.last_progress_output_write_offset);
}

void XmaContext::NoteProgress(const XMA_CONTEXT_DATA& data, const memory::RingBuffer& output_rb,
                              uint32_t previous_input_offset) {
  const auto previous = stall_tracker_.metrics();
  const uint8_t output_write_offset =
      static_cast<uint8_t>(output_rb.write_offset() / kOutputBytesPerBlock);
  if (!stall_tracker_.NoteProgress(data.input_buffer_read_offset, data.output_buffer_read_offset,
                                   output_write_offset)) {
    return;
  }
  PROFILE_XMA_STALL_RECOVERY();
  REXAPU_WARN(
      "XmaContext {}: recovered after stalls (no-space {}, no-progress {}): "
      "buffer {}, input {} -> {}, output {}/{}",
      id(), previous.consecutive_no_space_stalls, previous.consecutive_no_progress_stalls,
      data.current_buffer, previous_input_offset, data.input_buffer_read_offset,
      data.output_buffer_read_offset, output_write_offset);
}

void XmaContext::ResetStallMetrics() {
  stall_tracker_.Reset(kBitsPerPacketHeader);
}

uint8_t XmaContext::GetOutputPaddingHeadroom(uint8_t requested_padding,
                                             int32_t remaining_after_write) {
  if (!REXCVAR_GET(xma_relaxed_padding_admission)) {
    return requested_padding;
  }
  if (remaining_after_write <= 0) {
    return 0;
  }
  return static_cast<uint8_t>(std::min<int32_t>(requested_padding, remaining_after_write));
}

void XmaContext::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContext::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }

  const uint32_t loop_start = std::max(kBitsPerPacketHeader, data->loop_start);
  const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);

  if (data->input_buffer_read_offset != loop_end) {
    return;
  }

  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;

  if (data->loop_count < 255) {
    data->loop_count--;
  }
}

int XmaContext::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

int16_t XmaContext::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return static_cast<int16_t>(packet_number);
}

uint32_t XmaContext::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

const uint8_t* XmaContext::GetPacket(XMA_CONTEXT_DATA* data, const XmaPacketHandle& packet_handle) {
  if (!packet_handle.valid()) {
    return nullptr;
  }
  const uint32_t buffer_address =
      data->GetInputBufferAddress(static_cast<uint8_t>(packet_handle.buffer_index));
  return memory()->TranslatePhysical(buffer_address) + packet_handle.packet_index * kBytesPerPacket;
}

void XmaContext::WarnPacketResolution(const XmaPacketHandle& packet_handle,
                                      uint32_t logical_packet_index) {
  if (packet_handle.status == XmaPacketStatus::kValid ||
      packet_handle.status == XmaPacketStatus::kBufferInvalid) {
    return;
  }
  const uint32_t warning_bit = 1u << static_cast<uint32_t>(packet_handle.status);
  if (packet_warning_mask_ & warning_bit) {
    return;
  }
  packet_warning_mask_ |= warning_bit;
  REXAPU_WARN("XmaContext {}: cannot resolve logical packet {} (buffer {}, packet {}, status {})",
              id(), logical_packet_index, packet_handle.buffer_index, packet_handle.packet_index,
              static_cast<uint32_t>(packet_handle.status));
}

AssembledXmaPayload XmaContext::AssemblePacketPayloads(
    XMA_CONTEXT_DATA* data, uint32_t first_logical_packet, uint32_t current_input_packet_count,
    uint32_t frame_offset_in_packet, uint32_t bits_required, std::span<uint8_t> destination) {
  std::fill(destination.begin(), destination.end(), 0);

  AssembledXmaPayload result;
  if (frame_offset_in_packet < kBitsPerPacketHeader || frame_offset_in_packet >= kBitsPerPacket) {
    result.status = XmaPayloadStatus::kInvalidFrameOffset;
    return result;
  }
  if (bits_required == 0) {
    result.status = XmaPayloadStatus::kZeroLength;
    return result;
  }

  const uint64_t first_payload_bit =
      static_cast<uint64_t>(frame_offset_in_packet - kBitsPerPacketHeader);
  const uint64_t payload_bits_needed = first_payload_bit + bits_required;
  const uint64_t packet_count = (payload_bits_needed + kBitsPerPacketData - 1) / kBitsPerPacketData;
  const size_t destination_packet_capacity = destination.size() / kBytesPerPacketData;
  if (packet_count == 0 || packet_count > destination_packet_capacity ||
      packet_count > kMaxAssembledPackets) {
    result.status = XmaPayloadStatus::kCapacityExceeded;
    result.packet_count = static_cast<uint32_t>(packet_count);
    return result;
  }

  result.packet_count = static_cast<uint32_t>(packet_count);
  for (uint32_t i = 0; i < result.packet_count; ++i) {
    const uint32_t logical_packet = first_logical_packet + i;
    const XmaPacketHandle packet =
        ResolvePacket(*data, data->current_buffer, logical_packet, current_input_packet_count);
    if (!packet.valid()) {
      std::fill(destination.begin(), destination.end(), 0);
      result.status = XmaPayloadStatus::kPacketUnavailable;
      result.failed_packet = packet;
      WarnPacketResolution(packet, logical_packet);
      return result;
    }

    const uint8_t* packet_data = GetPacket(data, packet);
    if (!packet_data) {
      std::fill(destination.begin(), destination.end(), 0);
      result.status = XmaPayloadStatus::kPacketUnavailable;
      result.failed_packet = packet;
      return result;
    }
    std::memcpy(destination.data() + i * kBytesPerPacketData, packet_data + kBytesPerPacketHeader,
                kBytesPerPacketData);
  }

  result.valid_bits = result.packet_count * kBitsPerPacketData;
  result.status = XmaPayloadStatus::kValid;
  return result;
}

void XmaContext::WarnPayloadAssembly(const AssembledXmaPayload& payload,
                                     uint32_t first_logical_packet, uint32_t frame_offset_in_packet,
                                     uint32_t bits_required) {
  if (payload.valid()) {
    return;
  }
  const uint32_t warning_bit = 1u << static_cast<uint32_t>(payload.status);
  if (payload_warning_mask_ & warning_bit) {
    return;
  }
  payload_warning_mask_ |= warning_bit;
  REXAPU_WARN(
      "XmaContext {}: cannot assemble payload at packet {}, offset {}, bits {} "
      "(packets {}, status {})",
      id(), first_logical_packet, frame_offset_in_packet, bits_required, payload.packet_count,
      static_cast<uint32_t>(payload.status));
}

const uint8_t* XmaContext::GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                         uint32_t current_input_packet_count,
                                         XmaPacketHandle* packet_handle) {
  XmaPacketHandle resolved =
      ResolvePacket(*data, data->current_buffer, next_packet_index, current_input_packet_count);
  if (packet_handle) {
    *packet_handle = resolved;
  }
  WarnPacketResolution(resolved, next_packet_index);
  return GetPacket(data, resolved);
}

uint32_t XmaContext::GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                             uint32_t current_input_packet_count,
                                             uint32_t* resolved_packet_index, bool* packet_found) {
  *packet_found = false;
  while (next_packet_index < current_input_packet_count) {
    uint8_t* next_packet = buffer + (next_packet_index * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    if (packet_frame_offset <= kMaxFrameSizeinBits) {
      if (resolved_packet_index) {
        *resolved_packet_index = next_packet_index;
      }
      *packet_found = true;
      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;
    }
    next_packet_index++;
  }

  return kBitsPerPacketHeader;
}

uint32_t XmaContext::GetNextPacketReadOffset(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                             uint32_t current_input_packet_count,
                                             XmaPacketHandle* packet_handle) {
  XmaPacketHandle resolved =
      ResolvePacket(*data, data->current_buffer, next_packet_index, current_input_packet_count);
  if (!resolved.valid()) {
    WarnPacketResolution(resolved, next_packet_index);
    *packet_handle = resolved;
    return kBitsPerPacketHeader;
  }

  uint32_t resolved_packet_index = resolved.packet_index;
  bool packet_found = false;
  uint8_t* buffer = memory()->TranslatePhysical(
      data->GetInputBufferAddress(static_cast<uint8_t>(resolved.buffer_index)));
  uint32_t read_offset = GetNextPacketReadOffset(
      buffer, resolved.packet_index,
      data->GetInputBufferPacketCount(static_cast<uint8_t>(resolved.buffer_index)),
      &resolved_packet_index, &packet_found);
  if (packet_found) {
    resolved.packet_index = resolved_packet_index;
    *packet_handle = resolved;
    return read_offset;
  }

  // A scan that started in the active buffer may continue in the alternate
  // buffer. Resolve it explicitly so the returned offset and buffer identity
  // always describe the same packet.
  if (resolved.buffer_index == data->current_buffer) {
    resolved = ResolvePacket(*data, data->current_buffer, current_input_packet_count,
                             current_input_packet_count);
    if (!resolved.valid()) {
      WarnPacketResolution(resolved, current_input_packet_count);
      *packet_handle = resolved;
      return kBitsPerPacketHeader;
    }
    buffer = memory()->TranslatePhysical(
        data->GetInputBufferAddress(static_cast<uint8_t>(resolved.buffer_index)));
    resolved_packet_index = resolved.packet_index;
    read_offset = GetNextPacketReadOffset(
        buffer, resolved.packet_index,
        data->GetInputBufferPacketCount(static_cast<uint8_t>(resolved.buffer_index)),
        &resolved_packet_index, &packet_found);
    resolved.packet_index = resolved_packet_index;
  }

  if (!packet_found) {
    resolved.status = XmaPacketStatus::kPacketOutOfRange;
    WarnPacketResolution(resolved, next_packet_index);
  }

  *packet_handle = resolved;
  return read_offset;
}

memory::RingBuffer XmaContext::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    REXAPU_WARN(
        "XmaContext {}: Output buffer exceeds expected size! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      static_cast<int32_t>(output_rb.write_count()) / kOutputBytesPerBlock;

  return output_rb;
}

kPacketInfo XmaContext::GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      break;
    }

    stream.Advance(frame_size - 1);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    }
  }
  return packet_info;
}

void XmaContext::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                    const XMA_CONTEXT_DATA& initial_data, uint8_t* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;
  fresh.current_buffer = data.current_buffer;
  fresh.output_buffer_read_offset = data.output_buffer_read_offset;

  fresh.Store(context_ptr);
}

void XmaContext::Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return;
  }

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      remaining_subframe_blocks_in_output_buffer_ -=
          GetOutputPaddingHeadroom(static_cast<uint8_t>(data->output_buffer_padding),
                                   remaining_subframe_blocks_in_output_buffer_);
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return;
    }
  }

  const uint8_t effective_sdc = std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  int8_t subframes_to_write = std::min(static_cast<int8_t>(current_frame_remaining_subframes_),
                                       static_cast<int8_t>(effective_sdc));

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    const int8_t remaining_until_limit = static_cast<int8_t>(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  output_rb->Write(raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
                   subframes_to_write * kOutputBytesPerBlock);

  const int8_t headroom =
      (current_frame_remaining_subframes_ - subframes_to_write == 0)
          ? static_cast<int8_t>(GetOutputPaddingHeadroom(
                static_cast<uint8_t>(data->output_buffer_padding),
                remaining_subframe_blocks_in_output_buffer_ - subframes_to_write))
          : 0;

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;
}

int XmaContext::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  uint32_t channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->channels != static_cast<int>(channels)) {
    const bool is_stream_reinitialization =
        av_context_->sample_rate != 0 || av_context_->channels != 0;
    if (is_stream_reinitialization) {
      ResetStallMetrics();
    }
    REXAPU_NOISY_DEBUG("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}", id(),
                       av_context_->sample_rate, sample_rate, av_context_->channels, channels);
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

    av_context_->sample_rate = sample_rate;
    av_context_->channels = channels;
    av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      REXAPU_ERROR("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

void XmaContext::PreparePacket(uint32_t frame_size, uint32_t frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                      (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XmaContext::DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                              AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error sending packet for decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  return true;
}

void XmaContext::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  input_buffer_.fill(0);

  // Loop-end frame: decode it here (output limited to loop_subframe_end),
  // jump to loop_start afterwards in the next-offset step.
  bool is_loop_end_frame = false;
  if (data->loop_count > 0) {
    const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);
    is_loop_end_frame = (data->input_buffer_read_offset == loop_end);
  }

  if (!data->output_buffer_block_count) {
    REXAPU_ERROR("XmaContext {}: Error - Received 0 for output_buffer_block_count!", id());
    return;
  }

  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count = current_input_size / kBytesPerPacket;

  const int16_t packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    const uint32_t logical_packet_index = data->input_buffer_read_offset / kBitsPerPacket;
    XmaPacketHandle resolved = ResolvePacket(*data, data->current_buffer, logical_packet_index,
                                             current_input_packet_count);
    WarnPacketResolution(resolved, logical_packet_index);
    if (resolved.valid() && resolved.buffer_index != data->current_buffer) {
      const uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;
      SwapInputBuffer(data);
      data->input_buffer_read_offset =
          resolved.packet_index * kBitsPerPacket + std::max(kBitsPerPacketHeader, relative_offset);
    } else if (resolved.status == XmaPacketStatus::kBufferInvalid) {
      // The active buffer is logically exhausted and no continuation is ready.
      // Retiring it leaves Work() cleanly idle instead of re-entering forever.
      SwapInputBuffer(data);
    } else {
      data->error_status = 4;
    }
    return;
  }

  XmaPacketHandle current_packet =
      ResolvePacket(*data, data->current_buffer, packet_index, current_input_packet_count);
  WarnPacketResolution(current_packet, packet_index);
  uint8_t* packet = const_cast<uint8_t*>(GetPacket(data, current_packet));
  if (!packet) {
    data->error_status = 4;
    return;
  }
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip (0xFF) -- no new frames begin in this packet.
  if (skip_count == 0xFF) {
    XmaPacketHandle next_packet;
    uint32_t next_input_offset =
        GetNextPacketReadOffset(data, packet_index + 1, current_input_packet_count, &next_packet);
    if (next_packet.valid() && next_packet.buffer_index != data->current_buffer) {
      SwapInputBuffer(data);
    } else if (!next_packet.valid()) {
      if (next_packet.status == XmaPacketStatus::kBufferInvalid) {
        SwapInputBuffer(data);
      } else {
        data->error_status = 4;
      }
    }
    data->input_buffer_read_offset = next_input_offset;
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary.
  if (packet_info.current_frame_size_ == 0) {
    const AssembledXmaPayload header_payload =
        AssemblePacketPayloads(data, packet_index, current_input_packet_count, relative_offset,
                               kBitsPerFrameHeader, input_buffer_);
    if (!header_payload.valid()) {
      WarnPayloadAssembly(header_payload, packet_index, relative_offset, kBitsPerFrameHeader);
      data->error_status = 4;
      return;
    }

    BitStream combined(input_buffer_.data(), header_payload.valid_bits);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);
    packet_info.current_frame_size_ = static_cast<uint32_t>(combined.Peek(kBitsPerFrameHeader));
  }

  if (!IsValidFrameSize(packet_info.current_frame_size_)) {
    AssembledXmaPayload invalid_frame;
    invalid_frame.status = packet_info.current_frame_size_ == 0
                               ? XmaPayloadStatus::kZeroLength
                               : XmaPayloadStatus::kInvalidFrameSize;
    WarnPayloadAssembly(invalid_frame, packet_index, relative_offset,
                        packet_info.current_frame_size_);
    data->error_status = 4;
    return;
  }

  const AssembledXmaPayload frame_payload =
      AssemblePacketPayloads(data, packet_index, current_input_packet_count, relative_offset,
                             packet_info.current_frame_size_, input_buffer_);
  if (!frame_payload.valid()) {
    WarnPayloadAssembly(frame_payload, packet_index, relative_offset,
                        packet_info.current_frame_size_);
    data->error_status = 4;
    return;
  }

  BitStream stream(input_buffer_.data(), frame_payload.valid_bits);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  const uint32_t padding_start =
      static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  decoded_frame_.fill(0);

  if (PrepareDecoder(data->sample_rate, bool(data->is_stereo)) == 1) {
    // Reopening the codec restarts its output; the carried tail belongs to the
    // old stream.
    carry_valid_ = false;
  }
  PreparePacket(packet_info.current_frame_size_, padding_start);
  const bool decoded = DecodePacket(av_context_, av_packet_, av_frame_);
  if (decoded) {
    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data), bool(data->is_stereo),
                 decoded_frame_.data());

    // Realign the decoder's output onto the bitstream's sample numbering: the
    // samples of the frame just decoded run from kDecoderStartPadding into this
    // block and finish in the head of the next one.
    const size_t pad_bytes = kDecoderStartPadding * kBytesPerSample << data->is_stereo;
    const size_t carry_bytes = (kBytesPerFrameChannel << data->is_stereo) - pad_bytes;

    // Loop end: limit output to subframes 0..loop_subframe_end.
    const uint8_t decoded_output_limit =
        is_loop_end_frame ? static_cast<uint8_t>((data->loop_subframe_end + 1) << data->is_stereo)
                          : 0;
    // Loop start: skip leading subframes per loop_subframe_skip. skip == 4
    // means the whole frame is a warm-up frame (frame-aligned loop start):
    // decode seeds the codec state, output is fully discarded.
    const uint8_t decoded_start_skip =
        loop_start_skip_pending_ ? static_cast<uint8_t>(data->loop_subframe_skip << data->is_stereo)
                                 : 0;
    loop_start_skip_pending_ = false;

    if (carry_valid_) {
      std::memcpy(raw_frame_.data(), carry_frame_.data(), carry_bytes);
      std::memcpy(raw_frame_.data() + carry_bytes, decoded_frame_.data(), pad_bytes);

      current_frame_remaining_subframes_ = 4 << data->is_stereo;
      loop_frame_output_limit_ = pending_output_limit_;
      current_frame_remaining_subframes_ -=
          std::min(pending_start_skip_, current_frame_remaining_subframes_);
    }

    std::memcpy(carry_frame_.data(), decoded_frame_.data() + pad_bytes, carry_bytes);
    carry_valid_ = true;
    pending_output_limit_ = decoded_output_limit;
    pending_start_skip_ = decoded_start_skip;
  } else {
    // A dropped frame breaks the carry's adjacency; re-prime rather than splice
    // two blocks that are not neighbors.
    carry_valid_ = false;
  }

  // Compute where to go next.
  if (is_loop_end_frame) {
    UpdateLoopStatus(data);
    return;
  }

  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + packet_info.current_frame_size_) % kBitsPerPacket;
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  XmaPacketHandle next_packet;
  uint32_t next_input_offset =
      GetNextPacketReadOffset(data, next_packet_index, current_input_packet_count, &next_packet);

  if (next_packet.valid() && next_packet.buffer_index != data->current_buffer) {
    SwapInputBuffer(data);
  } else if (!next_packet.valid()) {
    if (next_packet.status == XmaPacketStatus::kBufferInvalid) {
      SwapInputBuffer(data);
    } else {
      data->error_status = 4;
    }
  }
  data->input_buffer_read_offset = next_input_offset;
}

void XmaContext::ConvertFrame(const uint8_t** samples, bool is_two_channel,
                              uint8_t* output_buffer) {
  // Loop through every sample, convert and drop it into the output array.
  // If more than one channel, we need to interleave the samples from each
  // channel next to each other. Always saturate because FFmpeg output is
  // not limited to [-1, 1] (for example 1.095 as seen in 5454082B).
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);

  // For testing of vectorized versions, stereo audio is common in 4D5307E6,
  // since the first menu frame; the intro cutscene also has more than 2
  // channels.
#if REX_ARCH_AMD64
  static_assert(kSamplesPerFrame % 8 == 0);
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  const __m128 scale_mm = _mm_set1_ps(scale);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    const __m128i shufmask = _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      // Load 8 samples, 4 for each channel.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Interleave channels and byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 4] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
    }
  } else {
    const __m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 8) {
      // Load 8 samples, as [in_channel_0 + i * 4] and
      // [in_channel_0 + i * 4 + 16] movups.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 2] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
    }
  }
#else
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      // Select the appropriate array based on the current channel.
      auto in = reinterpret_cast<const float*>(samples[j]);

      // Raw samples sometimes aren't within [-1, 1]
      float scaled_sample = rex::clamp_float(in[i], -1.0f, 1.0f) * scale;

      // Convert the sample and output it in big endian.
      auto sample = static_cast<int16_t>(scaled_sample);
      out[o++] = rex::byte_swap(sample);
    }
  }
#endif
}

}  // namespace rex::audio
