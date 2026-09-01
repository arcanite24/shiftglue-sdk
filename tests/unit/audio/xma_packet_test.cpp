/**
 * @file        xma_packet_test.cpp
 * @brief       XMA cross-buffer packet traversal regression tests
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/audio/xma/context.h>
#include <rex/cvar.h>
#include <rex/system/xmemory.h>

#include "test_memory.h"

namespace {

using rex::audio::ResolvePacket;
using rex::audio::XMA_CONTEXT_DATA;
using rex::audio::XmaContext;
using rex::audio::XmaNoSpaceObservation;
using rex::audio::XmaPacketStatus;
using rex::audio::XmaPayloadStatus;
using rex::audio::XmaStallTracker;

struct ResetRelaxedPaddingAdmission {
  ~ResetRelaxedPaddingAdmission() {
    rex::cvar::SetFlagByName("xma_relaxed_padding_admission", "false");
  }
};

std::array<uint8_t, sizeof(XMA_CONTEXT_DATA)> EmptyGuestContext() {
  return {};
}

XMA_CONTEXT_DATA MakeTwoBufferContext(uint32_t current_buffer = 0) {
  auto raw = EmptyGuestContext();
  XMA_CONTEXT_DATA data(raw.data());
  data.current_buffer = current_buffer;
  data.input_buffer_0_valid = 1;
  data.input_buffer_1_valid = 1;
  data.input_buffer_0_ptr = 0x10000;
  data.input_buffer_1_ptr = 0x20000;
  data.input_buffer_0_packet_count = 3;
  data.input_buffer_1_packet_count = 4;
  return data;
}

void SetPacketHeader(uint8_t* packet, uint8_t frame_count, uint32_t first_frame_offset,
                     uint8_t skip_count) {
  const uint32_t encoded_offset = first_frame_offset - XmaContext::kBitsPerPacketHeader;
  packet[0] = static_cast<uint8_t>((frame_count << 2) | ((encoded_offset >> 13) & 0x3));
  packet[1] = static_cast<uint8_t>(encoded_offset >> 5);
  packet[2] = static_cast<uint8_t>((encoded_offset << 3) | 1);
  packet[3] = skip_count;
}

void WriteCrossBufferPayloadBits(uint8_t* current, uint8_t* alternate, uint32_t first_payload_bit,
                                 uint32_t value, uint32_t bit_count) {
  for (uint32_t i = 0; i < bit_count; ++i) {
    const uint32_t assembled_bit = first_payload_bit + i;
    const uint32_t logical_packet = assembled_bit / XmaContext::kBitsPerPacketData;
    const uint32_t packet_bit = assembled_bit % XmaContext::kBitsPerPacketData;
    uint8_t* packet = logical_packet == 0
                          ? current
                          : alternate + (logical_packet - 1) * XmaContext::kBytesPerPacket;
    uint8_t& byte = packet[XmaContext::kBytesPerPacketHeader + packet_bit / 8];
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - packet_bit % 8));
    if (value & (1u << (bit_count - i - 1))) {
      byte |= mask;
    } else {
      byte &= static_cast<uint8_t>(~mask);
    }
  }
}

}  // namespace

TEST_CASE("XMA stall diagnostics remain bounded across transient episodes", "[audio][xma][stall]") {
  XmaStallTracker tracker;
  tracker.Reset(XmaContext::kBitsPerPacketHeader);

  uint32_t summary_count = 0;
  uint32_t recovery_log_count = 0;
  for (uint64_t total = 1; total <= 300; ++total) {
    const bool expected_summary = total == 1 || total == 8 || total == 64 || total == 256;
    const bool summary = tracker.NoteNoSpaceStall();
    INFO("stall total " << total);
    CHECK(summary == expected_summary);
    summary_count += summary ? 1 : 0;

    const bool recovery_log =
        tracker.NoteProgress(static_cast<uint32_t>(total), 0, static_cast<uint8_t>(total));
    CHECK(recovery_log == expected_summary);
    recovery_log_count += recovery_log ? 1 : 0;
  }

  CHECK(summary_count == 4);
  CHECK(recovery_log_count == 4);
  CHECK(tracker.metrics().total_no_space_stalls == 300);
  CHECK(tracker.metrics().total_recoveries == 300);
  CHECK(tracker.metrics().consecutive_no_space_stalls == 0);

  tracker.Reset(77);
  CHECK(tracker.metrics().total_no_space_stalls == 0);
  CHECK(tracker.metrics().total_recoveries == 0);
  CHECK(tracker.metrics().last_progress_input_offset == 77);
}

TEST_CASE("XMA output backpressure must repeat before becoming a stall", "[audio][xma][stall]") {
  XmaStallTracker tracker;
  tracker.Reset(XmaContext::kBitsPerPacketHeader);

  XmaNoSpaceObservation observation = {
      .input_offset = XmaContext::kBitsPerPacketHeader,
      .remaining_blocks = 2,
      .required_blocks = 6,
      .current_buffer = 0,
      .output_read_offset = 0,
      .output_write_offset = 4,
      .input_buffer_0_valid = true,
      .input_buffer_1_valid = false,
  };

  rex::audio::XmaNoSpaceObservationResult result;
  for (uint32_t attempt = 1; attempt < XmaStallTracker::kNoSpaceConfirmationObservations;
       ++attempt) {
    result = tracker.ObserveNoSpace(observation);
    INFO("observation " << attempt);
    CHECK_FALSE(result.repeated);
    CHECK_FALSE(result.log_recovery);
    CHECK(tracker.metrics().total_no_space_stalls == 0);
  }

  result = tracker.ObserveNoSpace(observation);
  CHECK(result.repeated);
  CHECK_FALSE(result.log_recovery);
  CHECK(tracker.NoteNoSpaceStall());
  CHECK(tracker.metrics().total_no_space_stalls == 1);

  ++observation.output_read_offset;
  ++observation.remaining_blocks;
  result = tracker.ObserveNoSpace(observation);
  CHECK_FALSE(result.repeated);
  CHECK(result.log_recovery);
  CHECK(tracker.metrics().total_recoveries == 1);

  ++observation.output_read_offset;
  ++observation.remaining_blocks;
  result = tracker.ObserveNoSpace(observation);
  CHECK_FALSE(result.repeated);
  CHECK_FALSE(result.log_recovery);
  CHECK(tracker.metrics().total_no_space_stalls == 1);
}

TEST_CASE("XMA strict padding admission reports no-space and recovers", "[audio][xma][stall]") {
  ResetRelaxedPaddingAdmission reset_padding;
  REQUIRE(rex::cvar::SetFlagByName("xma_relaxed_padding_admission", "false"));

  auto& memory = rex::testing::GetTestMemory();
  auto* virtual_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0x10000000));
  auto* physical_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(virtual_heap != nullptr);
  REQUIRE(physical_heap != nullptr);

  constexpr uint32_t kReadWrite =
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
  constexpr uint32_t kReserveCommit =
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
  uint32_t context_address = 0;
  uint32_t input_address = 0;
  uint32_t output_address = 0;
  REQUIRE(virtual_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &context_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &input_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &output_address));

  uint8_t* input = memory.TranslatePhysical(input_address);
  std::memset(input, 0, XmaContext::kBytesPerPacket);
  input[3] = 0xFF;

  auto* context_ptr = memory.TranslateVirtual(context_address);
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
  XMA_CONTEXT_DATA data(context_ptr);
  data.input_buffer_0_valid = 1;
  data.input_buffer_0_ptr = input_address;
  data.input_buffer_0_packet_count = 1;
  data.input_buffer_read_offset = XmaContext::kBitsPerPacketHeader;
  data.output_buffer_valid = 1;
  data.output_buffer_ptr = output_address;
  data.output_buffer_block_count = 8;
  data.output_buffer_read_offset = 0;
  data.output_buffer_write_offset = 4;
  data.subframe_decode_count = 4;
  data.output_buffer_padding = 2;
  data.Store(context_ptr);

  XmaContext context;
  REQUIRE(context.Setup(0, &memory, context_address) == 0);
  context.set_is_allocated(true);
  for (uint32_t attempt = 1; attempt < XmaStallTracker::kNoSpaceConfirmationObservations;
       ++attempt) {
    context.Enable();
    CHECK(context.Work());
    CHECK(context.stall_metrics().total_no_space_stalls == 0);
    CHECK(context.stall_metrics().total_recoveries == 0);
  }

  context.Enable();
  CHECK(context.Work());
  CHECK(context.stall_metrics().total_no_space_stalls == 1);
  CHECK(context.stall_metrics().total_recoveries == 0);

  REQUIRE(rex::cvar::SetFlagByName("xma_relaxed_padding_admission", "true"));
  context.Enable();
  CHECK(context.Work());
  CHECK(context.stall_metrics().total_no_space_stalls == 1);
  CHECK(context.stall_metrics().total_recoveries == 1);

  context.Clear();
  CHECK(context.stall_metrics().total_no_space_stalls == 0);
  CHECK(context.stall_metrics().total_recoveries == 0);
  context.Release();
  physical_heap->Release(output_address, nullptr);
  physical_heap->Release(input_address, nullptr);
  virtual_heap->Release(context_address, nullptr);
}

TEST_CASE("XMA no-progress diagnostics identify and recover malformed input",
          "[audio][xma][stall]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* virtual_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0x10000000));
  auto* physical_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(virtual_heap != nullptr);
  REQUIRE(physical_heap != nullptr);

  constexpr uint32_t kReadWrite =
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
  constexpr uint32_t kReserveCommit =
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
  uint32_t context_address = 0;
  uint32_t input_address = 0;
  uint32_t output_address = 0;
  REQUIRE(virtual_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &context_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &input_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &output_address));

  uint8_t* input = memory.TranslatePhysical(input_address);
  std::memset(input, 0, XmaContext::kBytesPerPacket);

  auto* context_ptr = memory.TranslateVirtual(context_address);
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
  XMA_CONTEXT_DATA data(context_ptr);
  data.input_buffer_0_valid = 1;
  data.input_buffer_0_ptr = input_address;
  data.input_buffer_0_packet_count = 1;
  data.input_buffer_read_offset = XmaContext::kBitsPerPacketHeader;
  data.output_buffer_valid = 1;
  data.output_buffer_ptr = output_address;
  data.output_buffer_block_count = 8;
  data.subframe_decode_count = 1;
  data.Store(context_ptr);

  XmaContext context;
  REQUIRE(context.Setup(1, &memory, context_address) == 0);
  context.set_is_allocated(true);
  context.Enable();
  CHECK(context.Work());
  CHECK(context.stall_metrics().total_no_progress_stalls == 1);
  CHECK(context.stall_metrics().total_recoveries == 0);
  XMA_CONTEXT_DATA malformed(context_ptr);
  CHECK(malformed.error_status == 4);

  input[3] = 0xFF;
  malformed.error_status = 0;
  malformed.output_buffer_valid = 1;
  malformed.Store(context_ptr);
  context.Enable();
  CHECK(context.Work());
  XMA_CONTEXT_DATA recovered(context_ptr);
  CAPTURE(context.stall_metrics().total_no_progress_stalls,
          context.stall_metrics().total_recoveries, recovered.input_buffer_0_valid,
          recovered.input_buffer_1_valid, recovered.current_buffer,
          recovered.input_buffer_read_offset, recovered.error_status);
  CHECK(context.stall_metrics().total_no_progress_stalls == 1);
  CHECK(context.stall_metrics().total_recoveries == 1);

  context.Release();
  physical_heap->Release(output_address, nullptr);
  physical_heap->Release(input_address, nullptr);
  virtual_heap->Release(context_address, nullptr);
}

TEST_CASE("XMA packet resolution preserves cross-buffer packet indices", "[audio][xma]") {
  XMA_CONTEXT_DATA data = MakeTwoBufferContext();

  SECTION("current buffer packet zero") {
    const auto packet = ResolvePacket(data, 0, 0, 3);
    CHECK(packet.valid());
    CHECK(packet.buffer_index == 0);
    CHECK(packet.packet_index == 0);
  }

  SECTION("current buffer last packet") {
    const auto packet = ResolvePacket(data, 0, 2, 3);
    CHECK(packet.valid());
    CHECK(packet.buffer_index == 0);
    CHECK(packet.packet_index == 2);
  }

  SECTION("alternate buffer packet zero") {
    const auto packet = ResolvePacket(data, 0, 3, 3);
    CHECK(packet.valid());
    CHECK(packet.buffer_index == 1);
    CHECK(packet.packet_index == 0);
  }

  SECTION("alternate buffer packet one and beyond") {
    const auto packet = ResolvePacket(data, 0, 5, 3);
    CHECK(packet.valid());
    CHECK(packet.buffer_index == 1);
    CHECK(packet.packet_index == 2);
  }

  SECTION("starting from buffer one") {
    const auto packet = ResolvePacket(data, 1, 5, 4);
    CHECK(packet.valid());
    CHECK(packet.buffer_index == 0);
    CHECK(packet.packet_index == 1);
  }
}

TEST_CASE("XMA packet resolution rejects unavailable guest packets", "[audio][xma]") {
  XMA_CONTEXT_DATA data = MakeTwoBufferContext();

  SECTION("alternate buffer invalid") {
    data.input_buffer_1_valid = 0;
    CHECK(ResolvePacket(data, 0, 3, 3).status == XmaPacketStatus::kBufferInvalid);
  }

  SECTION("alternate buffer has null address") {
    data.input_buffer_1_ptr = 0;
    CHECK(ResolvePacket(data, 0, 3, 3).status == XmaPacketStatus::kNullAddress);
  }

  SECTION("alternate buffer is shorter than the requested packet") {
    data.input_buffer_1_packet_count = 1;
    const auto packet = ResolvePacket(data, 0, 4, 3);
    CHECK(packet.status == XmaPacketStatus::kPacketOutOfRange);
    CHECK(packet.buffer_index == 1);
    CHECK(packet.packet_index == 1);
  }

  SECTION("caller packet count disagrees with context") {
    CHECK(ResolvePacket(data, 0, 0, 2).status == XmaPacketStatus::kInvalidContext);
  }
}

TEST_CASE("XMA payload assembly spans up to four packets", "[audio][xma]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* virtual_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0x10000000));
  auto* physical_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(virtual_heap != nullptr);
  REQUIRE(physical_heap != nullptr);

  constexpr uint32_t kReadWrite =
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
  constexpr uint32_t kReserveCommit =
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
  uint32_t context_address = 0;
  uint32_t current_address = 0;
  uint32_t alternate_address = 0;
  uint32_t output_address = 0;
  REQUIRE(virtual_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &context_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &current_address));
  REQUIRE(physical_heap->Alloc(8192, 4096, kReserveCommit, kReadWrite, false, &alternate_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &output_address));

  uint8_t* current = memory.TranslatePhysical(current_address);
  uint8_t* alternate = memory.TranslatePhysical(alternate_address);
  std::memset(current, 0, 4096);
  std::memset(alternate, 0, 8192);
  std::memset(current + XmaContext::kBytesPerPacketHeader, 0x11, XmaContext::kBytesPerPacketData);
  for (uint32_t packet = 0; packet < 3; ++packet) {
    std::memset(
        alternate + packet * XmaContext::kBytesPerPacket + XmaContext::kBytesPerPacketHeader,
        0x22 + packet, XmaContext::kBytesPerPacketData);
  }

  auto* context_ptr = memory.TranslateVirtual(context_address);
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
  XMA_CONTEXT_DATA data(context_ptr);
  data.current_buffer = 0;
  data.input_buffer_0_valid = 1;
  data.input_buffer_1_valid = 1;
  data.input_buffer_0_ptr = current_address;
  data.input_buffer_1_ptr = alternate_address;
  data.input_buffer_0_packet_count = 1;
  data.input_buffer_1_packet_count = 3;

  XmaContext context;
  REQUIRE(context.Setup(2, &memory, context_address) == 0);
  context.set_is_allocated(true);
  std::array<uint8_t, XmaContext::kBytesPerPacketData * XmaContext::kMaxAssembledPackets> assembled;

  SECTION("one packet") {
    const auto result =
        context.AssemblePacketPayloads(&data, 0, 1, XmaContext::kBitsPerPacketHeader, 8, assembled);
    CHECK(result.valid());
    CHECK(result.packet_count == 1);
    CHECK(result.valid_bits == XmaContext::kBitsPerPacketData);
    CHECK(assembled.front() == 0x11);
    CHECK(assembled[XmaContext::kBytesPerPacketData] == 0);
  }

  SECTION("two packets with a split header") {
    const auto result = context.AssemblePacketPayloads(&data, 0, 1, XmaContext::kBitsPerPacket - 7,
                                                       XmaContext::kBitsPerFrameHeader, assembled);
    CHECK(result.valid());
    CHECK(result.packet_count == 2);
    CHECK(assembled.front() == 0x11);
    CHECK(assembled[XmaContext::kBytesPerPacketData] == 0x22);
  }

  SECTION("three packets from a late frame start") {
    const auto result =
        context.AssemblePacketPayloads(&data, 0, 1, XmaContext::kBitsPerPacket - 7,
                                       XmaContext::kBitsPerPacketData + 16, assembled);
    CHECK(result.valid());
    CHECK(result.packet_count == 3);
    CHECK(assembled[XmaContext::kBytesPerPacketData] == 0x22);
    CHECK(assembled[XmaContext::kBytesPerPacketData * 2] == 0x23);
  }

  SECTION("four packets cross alternate packet zero and later packets") {
    const auto result = context.AssemblePacketPayloads(&data, 0, 1, XmaContext::kBitsPerPacket - 1,
                                                       0x7FFE, assembled);
    CHECK(result.valid());
    CHECK(result.packet_count == 4);
    CHECK(assembled.front() == 0x11);
    CHECK(assembled[XmaContext::kBytesPerPacketData] == 0x22);
    CHECK(assembled[XmaContext::kBytesPerPacketData * 2] == 0x23);
    CHECK(assembled[XmaContext::kBytesPerPacketData * 3] == 0x24);
  }

  SECTION("missing continuation buffer is rejected and zero-filled") {
    data.input_buffer_1_valid = 0;
    assembled.fill(0xAA);
    const auto result =
        context.AssemblePacketPayloads(&data, 0, 1, XmaContext::kBitsPerPacket - 7, 16, assembled);
    CHECK(result.status == XmaPayloadStatus::kPacketUnavailable);
    CHECK(result.failed_packet.status == XmaPacketStatus::kBufferInvalid);
    CHECK(
        std::all_of(assembled.begin(), assembled.end(), [](uint8_t value) { return value == 0; }));
  }

  SECTION("short alternate buffer is rejected") {
    data.input_buffer_1_packet_count = 1;
    const auto result =
        context.AssemblePacketPayloads(&data, 0, 1, XmaContext::kBitsPerPacket - 7,
                                       XmaContext::kBitsPerPacketData + 16, assembled);
    CHECK(result.status == XmaPayloadStatus::kPacketUnavailable);
    CHECK(result.failed_packet.status == XmaPacketStatus::kPacketOutOfRange);
    CHECK(result.failed_packet.packet_index == 1);
  }

  SECTION("requests beyond scratch capacity are rejected") {
    const auto result = context.AssemblePacketPayloads(
        &data, 0, 1, XmaContext::kBitsPerPacket - 1, XmaContext::kBitsPerPacketData * 4, assembled);
    CHECK(result.status == XmaPayloadStatus::kCapacityExceeded);
    CHECK(result.packet_count == 5);
  }

  SECTION("zero length and invalid offsets are rejected") {
    CHECK(
        context.AssemblePacketPayloads(&data, 0, 1, XmaContext::kBitsPerPacketHeader, 0, assembled)
            .status == XmaPayloadStatus::kZeroLength);
    CHECK(context.AssemblePacketPayloads(&data, 0, 1, 31, 8, assembled).status ==
          XmaPayloadStatus::kInvalidFrameOffset);
    CHECK(context.AssemblePacketPayloads(&data, 0, 1, XmaContext::kBitsPerPacket, 8, assembled)
              .status == XmaPayloadStatus::kInvalidFrameOffset);
  }

  SECTION("synthetic three-packet frame decodes and advances") {
    constexpr uint32_t kFrameOffset = XmaContext::kBitsPerPacket - 7;
    constexpr uint32_t kFrameSize = XmaContext::kBitsPerPacketData + 16;
    SetPacketHeader(current, 1, kFrameOffset, 2);
    SetPacketHeader(alternate + XmaContext::kBytesPerPacket * 2, 1,
                    XmaContext::kBitsPerPacketHeader, 0xFF);
    SetPacketHeader(alternate + XmaContext::kBytesPerPacket * 3, 1,
                    XmaContext::kBitsPerPacketHeader, 0xFF);
    WriteCrossBufferPayloadBits(current, alternate, kFrameOffset - XmaContext::kBitsPerPacketHeader,
                                kFrameSize, XmaContext::kBitsPerFrameHeader);

    data.input_buffer_1_packet_count = 4;
    data.input_buffer_read_offset = kFrameOffset;
    data.output_buffer_valid = 1;
    data.output_buffer_ptr = output_address;
    data.output_buffer_block_count = 4;
    data.output_buffer_write_offset = 1;
    data.subframe_decode_count = 1;
    data.Store(context_ptr);

    context.Enable();
    CHECK(context.Work());
    XMA_CONTEXT_DATA advanced(context_ptr);
    CHECK(advanced.error_status == 0);
    CHECK_FALSE(advanced.IsAnyInputBufferValid());
    CHECK(advanced.current_buffer == 0);
    CHECK(advanced.input_buffer_read_offset == XmaContext::kBitsPerPacketHeader);
  }

  SECTION("synthetic four-packet frame decodes and advances") {
    constexpr uint32_t kFrameOffset = XmaContext::kBitsPerPacket - 1;
    constexpr uint32_t kFrameSize = 0x7FFE;
    SetPacketHeader(current, 1, kFrameOffset, 3);
    SetPacketHeader(alternate + XmaContext::kBytesPerPacket * 3, 1,
                    XmaContext::kBitsPerPacketHeader, 0xFF);
    WriteCrossBufferPayloadBits(current, alternate, kFrameOffset - XmaContext::kBitsPerPacketHeader,
                                kFrameSize, XmaContext::kBitsPerFrameHeader);

    data.input_buffer_1_packet_count = 4;
    data.input_buffer_read_offset = kFrameOffset;
    data.output_buffer_valid = 1;
    data.output_buffer_ptr = output_address;
    data.output_buffer_block_count = 4;
    data.output_buffer_write_offset = 1;
    data.subframe_decode_count = 1;
    data.Store(context_ptr);

    context.Enable();
    CHECK(context.Work());
    XMA_CONTEXT_DATA advanced(context_ptr);
    CHECK(advanced.error_status == 0);
    CHECK_FALSE(advanced.IsAnyInputBufferValid());
    CHECK(advanced.current_buffer == 0);
    CHECK(advanced.input_buffer_read_offset == XmaContext::kBitsPerPacketHeader);
  }

  context.Release();
  physical_heap->Release(output_address, nullptr);
  physical_heap->Release(alternate_address, nullptr);
  physical_heap->Release(current_address, nullptr);
  virtual_heap->Release(context_address, nullptr);
}

TEST_CASE("XMA frame size rejects zero and the sentinel", "[audio][xma]") {
  CHECK_FALSE(XmaContext::IsValidFrameSize(0));
  CHECK(XmaContext::IsValidFrameSize(1));
  CHECK(XmaContext::IsValidFrameSize(0x7FFE));
  CHECK_FALSE(XmaContext::IsValidFrameSize(0x7FFF));
}

TEST_CASE("XMA exhausted input retires to a clean idle state", "[audio][xma]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);

  uint32_t context_address = 0;
  REQUIRE(heap->Alloc(
      4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &context_address));

  auto* context_ptr = memory.TranslateVirtual(context_address);
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
  XMA_CONTEXT_DATA data(context_ptr);
  data.input_buffer_0_valid = 1;
  data.input_buffer_0_ptr = 0x10000;
  data.input_buffer_0_packet_count = 1;
  data.input_buffer_read_offset = XmaContext::kBitsPerPacket;
  data.output_buffer_valid = 1;
  data.output_buffer_ptr = 0x30000;
  data.output_buffer_block_count = 4;
  data.output_buffer_write_offset = 1;
  data.subframe_decode_count = 1;
  data.Store(context_ptr);

  XmaContext context;
  REQUIRE(context.Setup(0, &memory, context_address) == 0);
  context.set_is_allocated(true);
  context.Enable();
  CHECK(context.Work());

  XMA_CONTEXT_DATA retired(context_ptr);
  CHECK_FALSE(retired.input_buffer_0_valid);
  CHECK_FALSE(retired.input_buffer_1_valid);
  CHECK(retired.current_buffer == 1);
  CHECK(retired.input_buffer_read_offset == XmaContext::kBitsPerPacketHeader);

  // Re-entry remains idle and leaves the retired state unchanged.
  context.Enable();
  CHECK(context.Work());
  XMA_CONTEXT_DATA idle(context_ptr);
  CHECK_FALSE(idle.IsAnyInputBufferValid());
  CHECK(idle.input_buffer_read_offset == XmaContext::kBitsPerPacketHeader);

  context.Release();
  heap->Release(context_address, nullptr);
}

TEST_CASE("XMA full-packet skip crosses to an alternate packet beyond zero", "[audio][xma]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* virtual_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0x10000000));
  auto* physical_heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(virtual_heap != nullptr);
  REQUIRE(physical_heap != nullptr);

  constexpr uint32_t kReadWrite =
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
  constexpr uint32_t kReserveCommit =
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
  uint32_t context_address = 0;
  uint32_t current_address = 0;
  uint32_t alternate_address = 0;
  uint32_t output_address = 0;
  REQUIRE(virtual_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &context_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &current_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &alternate_address));
  REQUIRE(physical_heap->Alloc(4096, 4096, kReserveCommit, kReadWrite, false, &output_address));

  uint8_t* current = memory.TranslatePhysical(current_address);
  uint8_t* alternate = memory.TranslatePhysical(alternate_address);
  std::memset(current, 0, XmaContext::kBytesPerPacket);
  std::memset(alternate, 0, XmaContext::kBytesPerPacket * 2);
  current[3] = 0xFF;

  // Packet 0 has an invalid first-frame offset, forcing the scan to packet 1.
  alternate[0] = 0x03;
  alternate[1] = 0xFF;
  alternate[2] = 0xF8;
  alternate[XmaContext::kBytesPerPacket + 3] = 0xFF;

  auto* context_ptr = memory.TranslateVirtual(context_address);
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
  XMA_CONTEXT_DATA data(context_ptr);
  data.input_buffer_0_valid = 1;
  data.input_buffer_1_valid = 1;
  data.input_buffer_0_ptr = current_address;
  data.input_buffer_1_ptr = alternate_address;
  data.input_buffer_0_packet_count = 1;
  data.input_buffer_1_packet_count = 2;
  data.input_buffer_read_offset = XmaContext::kBitsPerPacketHeader;
  data.output_buffer_valid = 1;
  data.output_buffer_ptr = output_address;
  data.output_buffer_block_count = 4;
  data.output_buffer_write_offset = 1;
  data.subframe_decode_count = 1;
  data.Store(context_ptr);

  XmaContext context;
  REQUIRE(context.Setup(1, &memory, context_address) == 0);
  context.set_is_allocated(true);
  context.Enable();
  CHECK(context.Work());

  XMA_CONTEXT_DATA retired(context_ptr);
  CHECK_FALSE(retired.IsAnyInputBufferValid());
  CHECK(retired.current_buffer == 0);
  CHECK(retired.input_buffer_read_offset == XmaContext::kBitsPerPacketHeader);
  CHECK(retired.error_status == 0);

  context.Release();
  physical_heap->Release(output_address, nullptr);
  physical_heap->Release(alternate_address, nullptr);
  physical_heap->Release(current_address, nullptr);
  virtual_heap->Release(context_address, nullptr);
}
