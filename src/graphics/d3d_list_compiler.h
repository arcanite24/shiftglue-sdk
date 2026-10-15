#pragma once

// DL-2.1 list compiler state (see d3d_list_compiler.cpp). Private to
// command_processor.cpp and d3d_list_compiler.cpp.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/register_file.h>

#include "d3d_shader_cache.h"

namespace rex::graphics {

struct CommandProcessor::D3dListCompiler {
  enum OpKind : uint32_t {
    kOpRegs,          // PacketWriteRegistersFromMem(first, words + payload, count)
    kOpRegsOne,       // count values into register first
    kOpShaderGuest,   // LoadGuestShader(first = type, payload = address, count = dwords)
    kOpShaderInline,  // LoadInlineShader(first = type, words + payload, count = dwords)
    kOpDraw,          // the draw packet at offset (predicate checked at the run)
    kOpPacket,        // ExecutePacket over the packet at offset
    kOpRegsMem,       // PacketWriteRegistersFromMem(first, physical payload, count)
    kOpBin,           // a bin mask or select packet (first = opcode)
    // Host plan only: WriteRegistersHost(first, plan_values + payload, count);
    // one register class, values in host order, redundant writes dropped.
    kOpHost,
  };
  // A type-3 packet with its predicate bit (type-0 headers use bit 0 for the
  // register index).
  static bool Predicated(uint32_t packet) { return (packet >> 30) == 3 && (packet & 1); }
  struct Op {
    uint32_t kind;
    uint32_t offset;   // the packet header's word offset
    uint32_t first;
    uint32_t count;
    uint32_t payload;  // word offset (or the shader address)
    uint32_t packet;   // the header
  };
  struct Check {
    uint32_t offset;
    uint32_t raw;  // the word as stored (big-endian)
  };
  struct List {
    std::vector<Op> ops;
    std::vector<Check> checks;
    uint32_t runs = 0;
    uint32_t last_swap = 0;
    uint32_t changes = 0;          // structure changes since the last sweep
    uint32_t backoff_until = 0;    // swap; decoded until then
    uint32_t draws = 0;
    bool compiled = false;
    bool uncompilable = false;
    // Attempt 2, page watch: the list's pages and their generations when it
    // was last validated (compiled or checked). While they hold, no guest
    // write reached the list, so neither its structure nor its values moved.
    uint32_t page_first = 0;
    uint32_t page_count = 0;
    std::vector<uint32_t> page_gens;
    bool watched = false;
    uint32_t dirty = 0;             // watched runs found written, this sweep
    uint32_t watch_off_until = 0;   // swap; checked every run until then
    // Host plan (gpu_d3d_list_host_regs), valid while watched: the ops with
    // register runs precomputed in host order, split by register class, and
    // the writes that repeat a value the list itself wrote earlier dropped.
    std::vector<Op> plan;
    std::vector<uint32_t> plan_values;
    uint32_t plan_written = 0;  // registers the plan writes
    uint32_t plan_elided = 0;   // register writes it drops
    bool plan_valid = false;
  };

  enum Fallback : uint32_t {
    kFirstRun,
    kChanged,
    kBackoff,
    kUncompilable,
    kTooLarge,
    kFallbackCount,
  };
  static constexpr const char* kFallbackName[] = {"first run", "structure changed",
                                                  "volatile backoff", "uncompilable",
                                                  "too large"};
  static constexpr uint32_t kMaxWords = 1u << 16;
  static constexpr uint32_t kSweepSwaps = 600;
  static constexpr uint32_t kVolatileChanges = 4;
  static constexpr uint32_t kBackoffSwaps = 600;
  // A watched list written this often in a sweep is checked instead.
  static constexpr uint32_t kVolatileDirty = 3;
  static constexpr uint32_t kWatchOffSwaps = 600;

  std::unordered_map<uint64_t, List> lists;
  uint32_t swaps = 0;
  uint32_t sweep_swaps = 0;

  struct Stats {
    uint64_t runs = 0;
    uint64_t replayed = 0;
    uint64_t compiles = 0;
    uint64_t fallback[kFallbackCount] = {};
    uint64_t fallback_draws[kFallbackCount] = {};
    uint64_t ops = 0;
    uint64_t generic_ops = 0;
    uint64_t draws = 0;
    uint64_t bailed = 0;
    uint64_t evicted = 0;
    // Validation (attempt 2).
    uint64_t clean = 0;         // runs validated by the page watch
    uint64_t checked = 0;       // runs validated by comparing every check word
    uint64_t check_words = 0;
    uint64_t dirty = 0;         // watched runs whose pages were written
    uint64_t watch_off = 0;     // lists put on checks for being written often
    uint64_t watch_misses = 0;  // verify: the watch said clean, the words differ
    // Host plan.
    uint64_t plan_runs = 0;
    uint64_t plan_builds = 0;
    uint64_t plan_registers = 0;  // written by the plan
    uint64_t plan_elided = 0;     // dropped by the plan
    // DL-2.2: replayed draws by tiling pass (untiled, first tile, later
    // tiles), those culled by their bin predicate, and those the backend
    // dropped for the untile pass (fh1_untile_predicated_tiling).
    uint64_t tile_draws[3] = {};
    uint64_t tile_culled[3] = {};
    uint64_t tile_untile_skipped[3] = {};
    // Verify.
    uint64_t verified = 0;
    uint64_t verified_draws = 0;
    uint64_t verified_registers = 0;
    uint64_t opaque = 0;
    uint64_t mismatch_lists = 0;
    uint64_t register_mismatches = 0;
    uint64_t draw_count_mismatches = 0;
    uint64_t load_mismatches = 0;
  } stats;
  uint32_t logs = 0;
  // Generic packets met (replay) or that ended the model (verify), by
  // 0x200 (type 1), 0x100 | opcode (predicated type 3) or opcode.
  uint64_t generic[0x201] = {};
  static uint32_t GenericKey(uint32_t packet) {
    if ((packet >> 30) == 1) return 0x200;
    return ((packet & 1) ? 0x100 : 0) | ((packet >> 8) & 0x7F);
  }

  // Plan building: the value the list itself last wrote to a register, while
  // known (stamp == plan_epoch).
  uint32_t plan_epoch = 0;
  std::unique_ptr<uint32_t[]> plan_stamp, plan_known;

  // Verify model.
  struct Load {
    uint32_t type, address, dwords;
  };
  struct Model {
    uint32_t epoch = 0;
    std::unique_ptr<uint32_t[]> values, stamp;
    std::vector<uint32_t> touched;
    // Checkpoints at each modelled draw: touched.size() and the values.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> draws;
    uint32_t modelled_draws = 0;  // draws before the model went opaque
    bool opaque = false;
    std::vector<Load> loads, seen_loads;
    uint32_t seen_draws = 0;
    uint32_t mismatches = 0;
    uint64_t key = 0;
  } model;

  void ModelWrite(uint32_t index, uint32_t value) {
    if (index >= RegisterFile::kRegisterCount) return;
    if (model.stamp[index] != model.epoch) {
      model.stamp[index] = model.epoch;
      model.touched.push_back(index);
    }
    model.values[index] = value;
  }
};

}  // namespace rex::graphics
