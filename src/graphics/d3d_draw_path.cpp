// DL-1.2 D3D-level draws: the command processor's half (see
// rex/system/d3d_draw.h and d3d_draw_window.h).
//
// A marker opens a window at the depth of the buffer it is in. The window
// ends at the next draw packet at that depth, which is the guest call's own
// draw: the call's prologue only writes state, shader loads, waits and
// coherency packets before it. A call that runs out of segment space
// continues in the next segment, so the window survives the return to the
// ring and resumes in the next buffer at its depth. Anything else that could
// not be part of the call (another marker, a nested buffer, a swap, too many
// packets) abandons the window; in draw mode the state the window skipped is
// applied first, so the packets that follow see what they would have seen.
//
// Verify mode skips nothing. At the draw packet it compares the decoder's
// registers with every register the description writes, the shader loads
// the window met with the description's, and the draw packet's words with
// the description's.
//
// Draw mode skips the packets whose effect the description holds (register
// and constant packets entirely inside its registers, shader loads of the
// types it loads) and runs the rest (waits, coherency, events, registers
// the description does not hold). At the draw packet it applies the
// description in the call's order and issues the draw from the
// description's words, at the place the guest's draw packet had in the
// stream.

#include <cstddef>
#include <cstring>
#include <type_traits>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/d3d_draw.h>

#include "d3d_draw_window.h"

namespace rex::graphics {

using namespace rex::graphics::xenos;
namespace d3d_draw = rex::system::d3d_draw;

namespace {

struct Entry {
  uint32_t kind;
  uint32_t count;
  uint32_t first;
  const uint32_t* payload;
};

// Calls fn(Entry) for each entry; false if the word stream is malformed.
template <typename Fn>
bool ForEachEntry(const d3d_draw::Description& description, Fn&& fn) {
  uint32_t i = 0;
  while (i + 2 <= description.word_count) {
    Entry entry{description.words[i] >> 16, description.words[i] & 0xFFFF,
                description.words[i + 1], description.words + i + 2};
    uint32_t payload_words = 0;
    switch (entry.kind) {
      case d3d_draw::kEntryRegisters:
      case d3d_draw::kEntryShader:
        payload_words = entry.count;
        break;
      case d3d_draw::kEntryRegistersFromMemory:
        payload_words = 1;
        break;
      default:
        return false;
    }
    if (i + 2 + payload_words > description.word_count) return false;
    fn(entry);
    i += 2 + payload_words;
  }
  return i == description.word_count;
}

constexpr const char* kAbortName[] = {"nested buffer", "marker", "swap", "packet limit",
                                      "predicated draw"};

// Fills the window's coverage from its description; false if the word
// stream is malformed. (A generic lambda: the window type is private.)
constexpr auto kCover = [](auto& w) {
  w.has_shader[0] = w.has_shader[1] = false;
  w.seen[0] = w.seen[1] = {};
  w.packets = 0;
  w.segments = 0;
  w.uncovered = 0;
  std::memset(w.covered, 0, sizeof(w.covered));
  return ForEachEntry(w.description, [&w](const Entry& entry) {
    if (entry.kind == d3d_draw::kEntryShader) {
      if (entry.first < 2) w.has_shader[entry.first] = true;
      return;
    }
    for (uint32_t i = 0; i < entry.count; ++i) {
      const uint32_t index = entry.first + i;
      if (index < std::decay_t<decltype(w)>::kCoveredRegisters) {
        w.covered[index >> 6] |= uint64_t(1) << (index & 63);
      }
    }
  });
};

}  // namespace

void CommandProcessor::D3dDrawMarker(uint32_t id, uint32_t mode) {
  if (!d3d_draw_) d3d_draw_ = std::make_unique<D3dDrawWindow>();
  D3dDrawWindow& w = *d3d_draw_;
  if (d3d_draw_open_) D3dDrawAbort(D3dDrawWindow::kAbortMarker);
  ++w.stats.markers;
  if (mode != d3d_draw::kModeVerify && mode != d3d_draw::kModeDraw) return;
  if (!d3d_draw::Fetch(id, &w.description)) {
    ++w.stats.fetch_failed;
    return;
  }
  if (w.description.flags) {
    ++w.stats.declined;
    return;
  }
  w.mode = mode;
  w.slice = false;
  w.depth = trace_ib_depth_;
  if (!kCover(w)) {
    ++w.stats.fetch_failed;
    return;
  }
  ++w.stats.opened;
  d3d_draw_open_ = true;
}

bool CommandProcessor::D3dDrawType0(memory::RingBuffer* reader, uint32_t packet) {
  D3dDrawWindow& w = *d3d_draw_;
  if (trace_ib_depth_ != w.depth) return false;
  if (++w.packets > D3dDrawWindow::kMaxPackets) {
    D3dDrawAbort(D3dDrawWindow::kAbortLimit);
    return false;
  }
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  const uint32_t first = packet & 0x7FFF;
  const bool one_register = (packet >> 15) & 1;
  if (w.mode != d3d_draw::kModeDraw) {
    if (w.slice && !w.CoveredRange(first, one_register ? 1 : count)) {
      if (w.uncovered < std::size(w.uncovered_keys)) w.uncovered_keys[w.uncovered] = first;
      ++w.uncovered;
    }
    return false;
  }
  if (!w.CoveredRange(first, one_register ? 1 : count)) {
    ++w.stats.kept_packets;
    return false;
  }
  ++w.stats.skipped_packets;
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::D3dDrawType3(memory::RingBuffer* reader, uint32_t packet, uint32_t opcode,
                                    uint32_t count) {
  D3dDrawWindow& w = *d3d_draw_;
  const bool draw = opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2;
  if (trace_ib_depth_ != w.depth) {
    if (trace_ib_depth_ < w.depth) {
      // Back in the ring between two segments: the call continues in the
      // next one.
      if (opcode == PM4_INDIRECT_BUFFER || opcode == PM4_INDIRECT_BUFFER_PFD) {
        ++w.segments;
        ++w.stats.carried;
      } else if (draw) {
        D3dDrawAbort(D3dDrawWindow::kAbortNestedIb);
      }
    }
    return false;
  }
  if (++w.packets > D3dDrawWindow::kMaxPackets) {
    D3dDrawAbort(D3dDrawWindow::kAbortLimit);
    return false;
  }
  const bool draw_mode = w.mode == d3d_draw::kModeDraw;
  // DL-2.1 verify: a packet of the call the replacement would not write.
  auto uncovered = [&w, opcode]() {
    if (!w.slice) return;
    if (w.uncovered < std::size(w.uncovered_keys)) {
      w.uncovered_keys[w.uncovered] = 0x10000 | opcode;
    }
    ++w.uncovered;
  };
  switch (opcode) {
    case PM4_NOP:
      if (count == 3) {
        memory::RingBuffer peek = *reader;
        const uint32_t magic = peek.ReadAndSwap<uint32_t>();
        if (magic == d3d_draw::kMarkerMagic || magic == d3d_draw::kReplaceMagic) {
          D3dDrawAbort(D3dDrawWindow::kAbortMarker);
        }
      }
      return false;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      D3dDrawAbort(D3dDrawWindow::kAbortNestedIb);
      return false;
    case PM4_XE_SWAP:
      D3dDrawAbort(D3dDrawWindow::kAbortSwap);
      return false;
    case PM4_DRAW_INDX:
    case PM4_DRAW_INDX_2:
      if (PredicatedOut(packet, opcode)) {
        D3dDrawAbort(D3dDrawWindow::kAbortPredicated);
        return false;
      }
      if (!draw_mode) {
        D3dDrawVerify(*reader, packet, opcode, count);
        d3d_draw_open_ = false;
        return false;
      }
      reader->AdvanceRead(count * sizeof(uint32_t));
      D3dDrawApplyState(w.description);
      D3dDrawIssue(w.description, packet);
      ++w.stats.drawn;
      d3d_draw_open_ = false;
      return true;
    case PM4_IM_LOAD:
    case PM4_IM_LOAD_IMMEDIATE: {
      memory::RingBuffer peek = *reader;
      const uint32_t word0 = peek.ReadAndSwap<uint32_t>();
      const uint32_t word1 = peek.ReadAndSwap<uint32_t>();
      const uint32_t type = opcode == PM4_IM_LOAD ? (word0 & 3) : word0;
      if (type > 1) {
        if (!draw_mode) uncovered();
        return false;
      }
      if (draw_mode) {
        if (!w.has_shader[type]) {
          ++w.stats.kept_packets;
          return false;
        }
        ++w.stats.skipped_packets;
        reader->AdvanceRead(count * sizeof(uint32_t));
        return true;
      }
      D3dDrawWindow::SeenLoad& seen = w.seen[type];
      ++seen.count;
      seen.dwords = word1 & 0xFFFF;
      if (opcode == PM4_IM_LOAD) {
        seen.address = word0 & ~uint32_t(3);
        seen.hash = 0;
      } else {
        seen.address = 0;
        seen.hash = XXH3_64bits(reinterpret_cast<const void*>(peek.read_ptr()),
                                size_t(seen.dwords) * sizeof(uint32_t));
      }
      return false;
    }
    case PM4_SET_CONSTANT:
    case PM4_SET_CONSTANT2:
    case PM4_LOAD_ALU_CONSTANT:
    case PM4_SET_SHADER_CONSTANTS: {
      if (!draw_mode) {
        uint32_t first = 0, registers = 0;
        if (!ConstantPacketRange(*reader, opcode, count, &first, &registers) ||
            !w.CoveredRange(first, registers)) {
          uncovered();
        }
        return false;
      }
      uint32_t first = 0, registers = 0;
      if (!ConstantPacketRange(*reader, opcode, count, &first, &registers) ||
          !w.CoveredRange(first, registers)) {
        ++w.stats.kept_packets;
        return false;
      }
      ++w.stats.skipped_packets;
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
    default:
      if (draw_mode) {
        ++w.stats.kept_packets;
      } else {
        uncovered();
      }
      return false;
  }
}

void CommandProcessor::D3dDrawAbort(uint32_t reason) {
  D3dDrawWindow& w = *d3d_draw_;
  ++w.stats.aborted[reason];
  if (w.mode == d3d_draw::kModeDraw) D3dDrawApplyState(w.description);
  d3d_draw_open_ = false;
}

void CommandProcessor::D3dDrawApplyState(const d3d_draw::Description& description) {
  ForEachEntry(description, [this](const Entry& entry) {
    switch (entry.kind) {
      case d3d_draw::kEntryRegisters:
        PacketWriteRegistersFromMem(entry.first, entry.payload, entry.count);
        break;
      case d3d_draw::kEntryRegistersFromMemory:
        PacketWriteRegistersFromMem(
            entry.first, memory_->TranslatePhysical<const uint32_t*>(entry.payload[0] & 0x3FFFFFFF),
            entry.count);
        break;
      case d3d_draw::kEntryShader: {
        const auto type = xenos::ShaderType(entry.first);
        if (entry.count > 2) {
          LoadInlineShader(type, entry.payload[0], entry.payload + 2, entry.payload[1]);
        } else {
          LoadGuestShader(type, entry.payload[0], entry.payload[1]);
        }
      } break;
    }
  });
}

void CommandProcessor::D3dDrawVerify(const memory::RingBuffer& reader, uint32_t packet,
                                     uint32_t opcode, uint32_t count) {
  D3dDrawWindow& w = *d3d_draw_;
  const d3d_draw::Description& description = w.description;
  ++w.stats.verified;
  if (w.slice) {
    ++w.stats.slice_verified;
    if (w.uncovered && w.segments) {
      ++w.stats.slice_carried_uncovered;
    } else if (w.uncovered) {
      ++w.stats.slice_uncovered_draws;
      w.stats.slice_uncovered_packets += w.uncovered;
      for (uint32_t i = 0; i < w.uncovered && i < std::size(w.uncovered_keys); ++i) {
        ++w.uncovered_packets[w.uncovered_keys[i]];
      }
    }
  }
  const RegisterFile& regs = decode_regs();
  uint32_t mismatches = 0;
  bool shader_mismatch = false;
  auto log_mismatch = [&](const char* what, uint32_t index, uint32_t expected, uint32_t actual) {
    if (w.mismatch_logs >= 64) return;
    ++w.mismatch_logs;
    REXGPU_INFO("DL-1.2 verify: draw {} {} {:04X} described {:08X} decoded {:08X}",
                description.id, what, index, expected, actual);
  };
  ForEachEntry(description, [&](const Entry& entry) {
    if (entry.kind == d3d_draw::kEntryShader) {
      if (entry.first > 1) return;
      const D3dDrawWindow::SeenLoad& seen = w.seen[entry.first];
      const uint32_t dwords = entry.payload[1];
      bool match = seen.count == 1 && seen.dwords == dwords;
      if (entry.count > 2) {
        match = match && seen.hash == XXH3_64bits(entry.payload + 2, size_t(dwords) * 4);
      } else {
        match = match && seen.address == entry.payload[0];
      }
      if (!match) {
        shader_mismatch = true;
        log_mismatch(entry.first ? "pixel shader" : "vertex shader", seen.count,
                     entry.payload[0], seen.address);
      }
      return;
    }
    const uint32_t* values = entry.payload;
    if (entry.kind == d3d_draw::kEntryRegistersFromMemory) {
      values = memory_->TranslatePhysical<const uint32_t*>(entry.payload[0] & 0x3FFFFFFF);
    }
    for (uint32_t i = 0; i < entry.count; ++i) {
      const uint32_t index = entry.first + i;
      if (index >= RegisterFile::kRegisterCount) continue;
      ++w.stats.verified_registers;
      const uint32_t expected = rex::byte_swap(values[i]);
      const uint32_t actual = regs.values[index];
      if (expected != actual) {
        ++mismatches;
        ++w.mismatch_registers[index];
        log_mismatch("register", index, expected, actual);
      }
    }
  });
  for (uint32_t type = 0; type < 2; ++type) {
    if (w.seen[type].count && !w.has_shader[type]) {
      shader_mismatch = true;
      log_mismatch(type ? "undescribed pixel shader load" : "undescribed vertex shader load",
                   w.seen[type].count, 0, w.seen[type].address);
    }
  }
  if (mismatches) {
    ++w.stats.register_mismatch_draws;
    w.stats.register_mismatches += mismatches;
  }
  if (shader_mismatch) ++w.stats.shader_mismatch_draws;
  // DL-1.2 descriptions hold the indexed packet's four words; a replaced
  // call's also its header (two words for the auto-index call).
  const uint32_t draw_words = (description.flags & d3d_draw::kFlagAutoIndex) ? 2 : 4;
  bool draw_match = opcode == PM4_DRAW_INDX && count >= draw_words;
  if (w.slice && packet != description.packet) {
    draw_match = false;
    log_mismatch("draw header", 0, description.packet, packet);
  }
  if (draw_match) {
    memory::RingBuffer peek = reader;
    for (uint32_t i = 0; i < draw_words; ++i) {
      const uint32_t word = peek.ReadAndSwap<uint32_t>();
      if (word != description.draw[i]) {
        draw_match = false;
        log_mismatch("draw word", i, description.draw[i], word);
      }
    }
  }
  if (!draw_match) ++w.stats.draw_mismatch_draws;
}

void CommandProcessor::D3dDrawIssue(const d3d_draw::Description& description, uint32_t packet) {
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = description.draw[1];
  DrawRecord record;
  record.packet = packet;
  record.initiator = initiator.value;
  record.fused_registers = 0;
  record.opcode_name = "D3D_DRAW";
  PacketWriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, initiator.value);
  if (description.flags & d3d_draw::kFlagAutoIndex) {
    // As PM4_DRAW_INDX with kAutoIndex: only the initiator is written.
    record.is_indexed = false;
    record.dma_base = 0;
    record.dma_size = 0;
    record.index_buffer_info.guest_base = 0;
    record.index_buffer_info.length = 0;
  } else {
    reg::VGT_DMA_SIZE dma_size;
    dma_size.value = description.draw[3];
    PacketWriteRegister(XE_GPU_REG_VGT_DMA_BASE, description.draw[2]);
    PacketWriteRegister(XE_GPU_REG_VGT_DMA_SIZE, dma_size.value);
    const uint32_t index_size =
        initiator.index_size == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
    record.is_indexed = true;
    record.dma_base = description.draw[2];
    record.dma_size = dma_size.value;
    record.index_buffer_info.guest_base = description.draw[2] & ~(index_size - 1);
    record.index_buffer_info.endianness = dma_size.swap_mode;
    record.index_buffer_info.format = initiator.index_size;
    record.index_buffer_info.length = dma_size.num_words * index_size;
    record.index_buffer_info.count = initiator.num_indices;
  }
  D3dDrawCountDraw();
  if (!record_split_) {
    ExecuteDrawRecord(record);
    return;
  }
  RecordWords& words = record_batch_->words;
  const size_t offset = words.size();
  words.resize(offset + 1 + kDrawRecordWords);
  words[offset] = kRecordDraw;
  std::memcpy(words.data() + offset + 1, &record, sizeof(record));
  if (++record_batch_draws_ >= 32) {
    PublishRecordBatch();
  }
}

void CommandProcessor::D3dSliceMarker(uint32_t offset, uint32_t id_mode) {
  if (!d3d_draw_) d3d_draw_ = std::make_unique<D3dDrawWindow>();
  D3dDrawWindow& w = *d3d_draw_;
  if (d3d_draw_open_) D3dDrawAbort(D3dDrawWindow::kAbortMarker);
  ++w.stats.slice_markers;
  const uint32_t mode = id_mode & 3;
  const d3d_draw::Description* description = d3d_draw::FetchArena(offset, id_mode >> 2);
  if (!description) {
    // Not expected: the game side publishes before it writes the marker,
    // and a chunk is kept until the decoder has fetched descriptions two
    // frames newer. In draw mode the call is then lost; counted.
    ++w.stats.slice_lost;
    return;
  }
  if (mode == d3d_draw::kSliceOn) {
    // The replaced call: its state flush, then its predicated draw.
    D3dDrawApplyState(*description);
    if (PredicatedOut(description->packet, PM4_DRAW_INDX)) {
      ++w.stats.slice_predicated_out;
      return;
    }
    if (d3d_skipped_loads_pending_) FlushSkippedShaderLoads();
    D3dDrawIssue(*description, description->packet);
    ++w.stats.slice_drawn;
    if (description->flags & d3d_draw::kFlagAutoIndex) ++w.stats.slice_auto_index;
    return;
  }
  if (mode != d3d_draw::kSliceVerify) return;
  std::memcpy(&w.description, description,
              offsetof(d3d_draw::Description, words) +
                  size_t(description->word_count) * sizeof(uint32_t));
  w.mode = d3d_draw::kModeVerify;
  w.slice = true;
  w.depth = trace_ib_depth_;
  if (!kCover(w)) {
    ++w.stats.slice_lost;
    return;
  }
  ++w.stats.slice_windows;
  d3d_draw_open_ = true;
}

void CommandProcessor::D3dDrawSwap() {
  D3dDrawWindow& w = *d3d_draw_;
  if (d3d_draw_open_) D3dDrawAbort(D3dDrawWindow::kAbortSwap);
  if (++w.stats.swaps < 600) return;
  const auto& s = w.stats;
  std::string aborted;
  for (uint32_t i = 0; i < D3dDrawWindow::kAbortCount; ++i) {
    if (s.aborted[i]) aborted += fmt::format(" {} {},", s.aborted[i], kAbortName[i]);
  }
  REXGPU_INFO(
      "DL-1.2 D3D draw ({} swaps): {} markers, {} unreadable, {} declined, {} windows ({} "
      "carried across segments), aborted:{}; verify: {} draws, {} registers checked, {} "
      "draws with register mismatches ({} registers), {} shader mismatches, {} draw packet "
      "mismatches; draw: {} drawn, {} packets skipped, {} kept",
      s.swaps, s.markers, s.fetch_failed, s.declined, s.opened, s.carried,
      aborted.empty() ? " none" : aborted, s.verified, s.verified_registers,
      s.register_mismatch_draws, s.register_mismatches, s.shader_mismatch_draws,
      s.draw_mismatch_draws, s.drawn, s.skipped_packets, s.kept_packets);
  if (s.slice_markers) {
    REXGPU_INFO(
        "DL-2.1 slice ({} swaps): {} markers, {} lost, {} drawn ({} auto-index), {} predicated "
        "out; verify: {} windows, {} calls verified, {} with undescribed packets ({} packets), "
        "{} more carried across segments (kick packets)",
        s.swaps, s.slice_markers, s.slice_lost, s.slice_drawn, s.slice_auto_index,
        s.slice_predicated_out, s.slice_windows, s.slice_verified, s.slice_uncovered_draws,
        s.slice_uncovered_packets, s.slice_carried_uncovered);
  }
  if (!w.uncovered_packets.empty()) {
    std::string top;
    uint32_t n = 0;
    for (const auto& [key, packets] : w.uncovered_packets) {
      if (n++ >= 32) break;
      top += key >> 16 ? fmt::format(" op{:02X}x{}", key & 0xFF, packets)
                       : fmt::format(" {:04X}x{}", key, packets);
    }
    REXGPU_INFO("DL-2.1 verify: undescribed packets:{}", top);
  }
  if (!w.mismatch_registers.empty()) {
    std::string top;
    uint32_t n = 0;
    for (const auto& [index, mismatch_count] : w.mismatch_registers) {
      if (n++ >= 32) break;
      top += fmt::format(" {:04X}x{}", index, mismatch_count);
    }
    REXGPU_INFO("DL-1.2 verify: mismatching registers:{}", top);
  }
  w.stats = {};
  w.mismatch_registers.clear();
  w.uncovered_packets.clear();
}

}  // namespace rex::graphics
