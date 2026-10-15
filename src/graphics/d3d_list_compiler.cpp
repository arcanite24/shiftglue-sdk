// DL-2.1 list compiler (Pinyon Shift docs/D3D_RENDERER_RESEARCH.md): nested
// indirect buffers, the title's display lists, run from a compiled form.
//
// A list is compiled the second time a buffer (address, size) runs: its
// packets are parsed once into operations (register runs, single-register
// runs, shader loads, draws, and generic packets for everything else) and a
// set of checks, the raw words the parse depended on (packet headers and the
// control words that pick the registers or the shader). At each run the
// checks are compared first; any difference means another write changed
// the list's structure, so the list is decoded as usual and recompiled at
// its next run. Lists whose structure changes repeatedly are left to the
// decoder for a while (volatile backoff).
//
// What the checks do not cover is read from the buffer at the run as it is:
// register values, draw words, inline microcode. A fixup (the title's patch
// of a list value before a run, through RunCommandBuffer's dispatcher) is
// therefore a binding, not a change. The replay calls the decoder's own
// register, shader-load and draw functions with the same arguments the
// packet handlers would, in the same order, so the state each list inherits
// from the stream (registers, bin mask and select, shaders) is the decoder's
// as usual: the replay has no state of its own. Generic packets run through
// ExecutePacket; if one opens a DL-1.2 window (a marker), the rest of the
// list is decoded.
//
// Verify mode compiles and models each list run (the register values the
// operations write, the shader loads, the draws) and then decodes it as
// usual, comparing the decoder's registers with the model at each draw and
// at the end, the loads and the number of draws. A generic packet that
// could write registers makes the rest of the run unmodelled.
//
// Attempt 2 (each part hot and separately switchable):
// - Page watch (gpu_d3d_list_watch): a list's pages are write-watched once it
//   compiles. While no guest write reached them, the list is as it was when
//   last validated, and the run reads none of its words to validate it. A
//   written list is checked word by word once (attempt 1's checks), then
//   watched again; one written about every run is left to the checks.
// - Host plan (gpu_d3d_list_host_regs): for a watched list, the register
//   runs are precomputed once in host order, split by register class, and
//   writes that repeat a value the list itself wrote earlier (with nothing
//   between that could change it) are dropped; the replay hands the runs to
//   WriteRegistersHost. The register file, its state hash and the backend's
//   dirty tracking end each draw as the PM4 path leaves them: a dropped write
//   stored the value the register already held.
// The page watch and the shader-load cache it was built with are in
// d3d_shader_cache.cpp.

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/d3d_draw.h>

#include "d3d_census_decoder.h"
#include "d3d_list_compiler.h"

REXCVAR_DEFINE_INT32(gpu_d3d_list_compile, -1, "GPU",
                     "D3D_RENDERER_RESEARCH DL-2.1: run nested command buffers (display lists) "
                     "from a compiled form checked against their packet headers and control "
                     "words at each run (-1: follow pinyon_shift_d3d_slice; 0: off; 1: verify, "
                     "compile and model each run, then decode it and compare; 2: on). Read at "
                     "each swap. Off while the D3D census, the replay probe, IB statistics or "
                     "bin tracing are on")
    .range(-1, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(gpu_d3d_list_watch, -1, "GPU",
                     "D3D_RENDERER_RESEARCH DL-2.1 attempt 2: with gpu_d3d_list_compile, "
                     "validate a compiled list by a write watch on its pages instead of "
                     "comparing its words at every run (-1: on with the compiler; 0: compare "
                     "every run; 1: watch). Read at each swap")
    .range(-1, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(gpu_d3d_list_host_regs, -1, "GPU",
                     "D3D_RENDERER_RESEARCH DL-2.1 attempt 2: replay the register writes of a "
                     "watched compiled list from a precomputed plan (host order, split by "
                     "register class, repeated values dropped) instead of from its packets "
                     "(-1: on with the watch; 0: off; 1: on). Not with gpu_record_thread. Read "
                     "at each swap")
    .range(-1, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(int32_t, gpu_trace_bins_vblank);
REXCVAR_DECLARE(bool, gpu_ib_identity_stats);
REXCVAR_DECLARE(bool, gpu_template_stats);
REXCVAR_DECLARE(int32_t, gpu_replay_floor_probe);

namespace rex::graphics {

using namespace rex::graphics::xenos;
namespace d3d_draw = rex::system::d3d_draw;

namespace {

uint32_t Swap(uint32_t raw) { return rex::byte_swap(raw); }

constexpr uint32_t kConstantBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};

// Generic packets the verify model can step over: they write no compared
// register and do not change the bin mask or select (the model evaluates
// predicates as at the start of the run).
bool GenericKeepsRegisters(uint32_t packet) {
  if ((packet >> 30) != 3) return false;
  switch ((packet >> 8) & 0x7F) {
    case PM4_NOP:
    // Writes only COHER_STATUS_HOST (MakeCoherent), which is not compared.
    case PM4_WAIT_REG_MEM:
      return true;
    default:
      return false;
  }
}

// Parses the buffer into ops and checks; false if a packet is malformed or
// overruns the buffer (the decoder then reports it as usual).
template <typename List, typename Op>
bool CompileList(const uint32_t* words, uint32_t count, List& list) {
  list.ops.clear();
  list.checks.clear();
  list.draws = 0;
  auto check = [&](uint32_t offset) { list.checks.push_back({offset, words[offset]}); };
  auto op = [&](uint32_t kind, uint32_t offset, uint32_t first, uint32_t n, uint32_t payload,
                uint32_t packet) { list.ops.push_back(Op{kind, offset, first, n, payload, packet}); };
  uint32_t i = 0;
  while (i < count) {
    const uint32_t packet = Swap(words[i]);
    check(i);
    if (packet == 0) {
      ++i;
      continue;
    }
    switch (packet >> 30) {
      case 0: {
        const uint32_t n = ((packet >> 16) & 0x3FFF) + 1;
        if (uint64_t(i) + 1 + n > count) return false;
        const uint32_t first = packet & 0x7FFF;
        op((packet >> 15) & 1 ? 1u : 0u, i, first, n, i + 1, packet);
        i += 1 + n;
      } break;
      case 1:
        if (uint64_t(i) + 3 > count) return false;
        op(5, i, 0, 3, 0, packet);
        i += 3;
        break;
      case 2:
        ++i;
        break;
      case 3: {
        const uint32_t opcode = (packet >> 8) & 0x7F;
        const uint32_t n = ((packet >> 16) & 0x3FFF) + 1;
        if (uint64_t(i) + 1 + n > count) return false;
        const uint32_t size = 1 + n;
        const bool predicated = packet & 1;
        if (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2) {
          op(4, i, opcode, n, i + 1, packet);
          ++list.draws;
        } else if (predicated && opcode != PM4_SET_CONSTANT && opcode != PM4_SET_CONSTANT2 &&
                   opcode != PM4_SET_SHADER_CONSTANTS && opcode != PM4_LOAD_ALU_CONSTANT) {
          // Register packets keep their predicate (checked at the run).
          op(5, i, 0, size, 0, packet);
        } else {
          switch (opcode) {
            case PM4_SET_CONSTANT: {
              const uint32_t offset_type = Swap(words[i + 1]);
              const uint32_t type = (offset_type >> 16) & 0xFF;
              check(i + 1);
              if (type < std::size(kConstantBase)) {
                op(0, i, kConstantBase[type] + (offset_type & 0x7FF), n - 1, i + 2, packet);
              } else {
                op(5, i, 0, size, 0, packet);
              }
            } break;
            case PM4_SET_CONSTANT2:
            case PM4_SET_SHADER_CONSTANTS:
              check(i + 1);
              op(0, i, Swap(words[i + 1]) & 0xFFFF, n - 1, i + 2, packet);
              break;
            case PM4_LOAD_ALU_CONSTANT: {
              // Constants from memory: address, offset and type, size.
              if (n < 3) return false;
              check(i + 1);
              check(i + 2);
              check(i + 3);
              const uint32_t offset_type = Swap(words[i + 2]);
              const uint32_t type = (offset_type >> 16) & 0xFF;
              if (type < std::size(kConstantBase)) {
                op(6, i, kConstantBase[type] + (offset_type & 0x7FF),
                   Swap(words[i + 3]) & 0xFFF, Swap(words[i + 1]) & 0x3FFFFFFF, packet);
              } else {
                op(5, i, 0, size, 0, packet);
              }
            } break;
            case PM4_IM_LOAD: {
              if (n < 2) return false;
              check(i + 1);
              check(i + 2);
              const uint32_t addr_type = Swap(words[i + 1]);
              const uint32_t type = addr_type & 3;
              if (type > 1) {
                op(5, i, 0, size, 0, packet);
              } else {
                op(2, i, type, Swap(words[i + 2]) & 0xFFFF, addr_type & ~uint32_t(3), packet);
              }
            } break;
            case PM4_IM_LOAD_IMMEDIATE: {
              if (n < 2) return false;
              check(i + 1);
              check(i + 2);
              const uint32_t type = Swap(words[i + 1]);
              const uint32_t dwords = Swap(words[i + 2]) & 0xFFFF;
              if (type > 1 || dwords > n - 2) {
                op(5, i, 0, size, 0, packet);
              } else {
                op(3, i, type, dwords, i + 3, packet);
              }
            } break;
            case PM4_NOP:
              // A three-word NOP may be a marker.
              if (n == 3) op(5, i, 0, size, 0, packet);
              break;
            case PM4_INVALIDATE_STATE:
              break;
            case PM4_SET_BIN_MASK_LO:
            case PM4_SET_BIN_MASK_HI:
            case PM4_SET_BIN_SELECT_LO:
            case PM4_SET_BIN_SELECT_HI:
              op(7, i, opcode, 1, i + 1, packet);
              break;
            case PM4_SET_BIN_MASK:
              if (n != 2) return false;
              op(7, i, opcode, 2, i + 1, packet);
              break;
            default:
              op(5, i, 0, size, 0, packet);
              break;
          }
        }
        i += size;
      } break;
    }
  }
  return true;
}

}  // namespace

void CommandProcessor::ExecutePacketsFrom(const uint32_t* base, uint32_t count,
                                          uint32_t offset) {
  if (offset >= count) return;
  memory::RingBuffer reader(reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(base)),
                            count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  reader.set_read_offset(offset * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      break;
    }
  } while (reader.read_count());
}

namespace {

// Writes the host plan never drops: they act on more than the register value,
// or are per-draw registers the draw packet writes itself.
bool PlanNeverElides(uint32_t index) {
  return (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) ||
         index == XE_GPU_REG_COHER_STATUS_HOST ||
         (index >= XE_GPU_REG_DC_LUT_RW_INDEX && index <= XE_GPU_REG_DC_LUT_30_COLOR) ||
         // CommandProcessor::IsPerDrawRegister's set.
         (index >= XE_GPU_REG_VGT_EVENT_INITIATOR && index <= XE_GPU_REG_VGT_DRAW_INITIATOR) ||
         (index >= XE_GPU_REG_COHER_SIZE_HOST && index <= XE_GPU_REG_COHER_STATUS_HOST) ||
         (index >= 0x01C0 && index <= 0x01FF) || (index >= 0x1800 && index <= 0x19FF) ||
         index >= 0x5000;
}

// The backends' register classes (float, fetch, bool and loop constants);
// a plan run never crosses one, so it takes one bulk path.
uint32_t RegisterClassEnd(uint32_t index) {
  static constexpr uint32_t kBoundaries[] = {0x4000, 0x4800, 0x48C0, 0x4900, 0x4928};
  for (uint32_t boundary : kBoundaries) {
    if (index < boundary) return boundary;
  }
  return UINT32_MAX;
}

template <typename Compiler>
void BuildPlan(Compiler& c, typename Compiler::List& list, const uint32_t* words) {
  using Op = typename Compiler::Op;
  using D3dListCompiler = Compiler;
  constexpr uint32_t kRegisters = RegisterFile::kRegisterCount;
  if (!c.plan_stamp) {
    c.plan_stamp.reset(new uint32_t[kRegisters]());
    c.plan_known.reset(new uint32_t[kRegisters]);
  }
  auto new_epoch = [&]() {
    if (++c.plan_epoch == 0) {
      std::memset(c.plan_stamp.get(), 0, kRegisters * sizeof(uint32_t));
      c.plan_epoch = 1;
    }
  };
  new_epoch();
  list.plan.clear();
  list.plan_values.clear();
  list.plan.reserve(list.ops.size());
  auto forget = [&](uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count && first + i < kRegisters; ++i) c.plan_stamp[first + i] = 0;
  };
  uint32_t written = 0, elided = 0;
  // Values for [first, first + count): runs of the writes that change what
  // the list itself last wrote, split by register class.
  auto emit = [&](const Op& source, uint32_t first, const uint32_t* values, uint32_t count,
                  bool swapped) {
    uint32_t run_first = 0, run_count = 0, run_end = 0;
    auto flush = [&]() {
      if (!run_count) return;
      list.plan.push_back(Op{D3dListCompiler::kOpHost, source.offset, run_first, run_count,
                             uint32_t(list.plan_values.size() - run_count), source.packet});
      written += run_count;
      run_count = 0;
    };
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t index = first + i;
      const uint32_t value = swapped ? Swap(values[i]) : values[i];
      const bool never = PlanNeverElides(index);
      if (!never && c.plan_stamp[index] == c.plan_epoch && c.plan_known[index] == value) {
        flush();
        ++elided;
        continue;
      }
      if (never) {
        c.plan_stamp[index] = 0;
      } else {
        c.plan_stamp[index] = c.plan_epoch;
        c.plan_known[index] = value;
      }
      if (run_count && index >= run_end) flush();
      if (!run_count) {
        run_first = index;
        run_end = RegisterClassEnd(index);
      }
      list.plan_values.push_back(value);
      ++run_count;
    }
    flush();
  };
  for (const Op& op : list.ops) {
    switch (op.kind) {
      case D3dListCompiler::kOpRegs:
        if (D3dListCompiler::Predicated(op.packet) || uint64_t(op.first) + op.count > kRegisters) {
          list.plan.push_back(op);
          forget(op.first, op.count);
        } else {
          emit(op, op.first, words + op.payload, op.count, true);
        }
        break;
      case D3dListCompiler::kOpRegsOne:
        if (!op.count) break;
        if (op.first >= kRegisters || PlanNeverElides(op.first)) {
          list.plan.push_back(op);
          forget(op.first, 1);
        } else {
          // Plain state: only the last value lands.
          emit(op, op.first, words + op.payload + op.count - 1, 1, true);
        }
        break;
      case D3dListCompiler::kOpRegsMem:
        list.plan.push_back(op);
        forget(op.first, op.count);
        break;
      case D3dListCompiler::kOpPacket:
        list.plan.push_back(op);
        if (!GenericKeepsRegisters(op.packet)) new_epoch();
        break;
      default:
        list.plan.push_back(op);
        break;
    }
  }
  list.plan_written = written;
  list.plan_elided = elided;
  list.plan_valid = true;
  ++c.stats.plan_builds;
}

}  // namespace

bool CommandProcessor::D3dListExecute(uint32_t ptr, uint32_t count) {
  D3dListCompiler& c = *d3d_lists_;  // made at the swap that set the mode
  using Op = D3dListCompiler::Op;
  ++c.stats.runs;
  if (count > D3dListCompiler::kMaxWords || !count) {
    ++c.stats.fallback[D3dListCompiler::kTooLarge];
    return false;
  }
  const uint64_t key = (uint64_t(ptr) << 32) | count;
  D3dListCompiler::List& list = c.lists[key];
  list.last_swap = c.swaps;
  ++list.runs;
  auto fallback = [&](uint32_t reason) {
    ++c.stats.fallback[reason];
    c.stats.fallback_draws[reason] += list.draws;
    return false;
  };
  if (int32_t(list.backoff_until - c.swaps) > 0) return fallback(D3dListCompiler::kBackoff);
  const uint32_t* words = memory_->TranslatePhysical<const uint32_t*>(ptr);

  // Validation. With the page watch, a list whose pages no guest write has
  // reached since it was last validated is as it was then: no word needs
  // reading. Otherwise every check word is compared (attempt 1). The
  // snapshot and the arming come before the words are read, so a write
  // after this point shows at the next run.
  const bool watchable =
      d3d_list_watch_ && int32_t(list.watch_off_until - c.swaps) <= 0;
  auto snapshot_and_arm = [&]() {
    D3dPageWatch& watch = *d3d_page_watch_;
    const uint32_t physical = ptr & 0x1FFFFFFFu;
    list.page_first = physical >> D3dPageWatch::kPageShift;
    list.page_count =
        ((physical + count * 4 - 1) >> D3dPageWatch::kPageShift) - list.page_first + 1;
    list.page_gens.resize(list.page_count);
    for (uint32_t i = 0; i < list.page_count; ++i) {
      list.page_gens[i] = watch.Gen(list.page_first + i);
    }
    watch.Arm(list.page_first, list.page_count);
  };
  bool clean = false;
  if (list.compiled) {
    if (list.watched && d3d_list_watch_) {
      clean = true;
      for (uint32_t i = 0; i < list.page_count; ++i) {
        if (d3d_page_watch_->Gen(list.page_first + i) != list.page_gens[i]) {
          clean = false;
          break;
        }
      }
      if (!clean) {
        ++c.stats.dirty;
        if (++list.dirty >= D3dListCompiler::kVolatileDirty) {
          // Written about every run: the watch costs a fault each time.
          list.watch_off_until = c.swaps + D3dListCompiler::kWatchOffSwaps;
          ++c.stats.watch_off;
        }
      }
      if (clean && d3d_list_mode_ == d3d_draw::kSliceVerify) {
        // The watch must agree with the words.
        for (const D3dListCompiler::Check& check : list.checks) {
          if (words[check.offset] != check.raw) {
            ++c.stats.watch_misses;
            clean = false;
            if (c.logs < 64) {
              ++c.logs;
              REXGPU_INFO("DL-2.1 list watch miss: list {:08X}+{} word {} changed unseen", ptr,
                          count, check.offset);
            }
            break;
          }
        }
      }
    }
    if (clean) {
      ++c.stats.clean;
    } else {
      list.watched = false;
      list.plan_valid = false;
      const bool rewatch = watchable && int32_t(list.watch_off_until - c.swaps) <= 0;
      if (rewatch) snapshot_and_arm();
      ++c.stats.checked;
      c.stats.check_words += list.checks.size();
      for (const D3dListCompiler::Check& check : list.checks) {
        if (words[check.offset] != check.raw) {
          // Another write changed the structure: decode, recompile next run.
          list.compiled = false;
          if (++list.changes >= D3dListCompiler::kVolatileChanges) {
            list.backoff_until = c.swaps + D3dListCompiler::kBackoffSwaps;
          }
          return fallback(D3dListCompiler::kChanged);
        }
      }
      list.watched = rewatch;
    }
  } else {
    if (list.runs < 2) return fallback(D3dListCompiler::kFirstRun);
    if (list.uncompilable) return fallback(D3dListCompiler::kUncompilable);
    list.watched = false;
    list.plan_valid = false;
    if (watchable) snapshot_and_arm();
    ++c.stats.compiles;
    if (!CompileList<D3dListCompiler::List, Op>(words, count, list)) {
      list.uncompilable = true;
      list.ops.clear();
      list.checks.clear();
      return fallback(D3dListCompiler::kUncompilable);
    }
    list.compiled = true;
    list.watched = watchable;
  }
  // The host plan holds the values as validated, so only while watched.
  const bool use_plan = d3d_list_host_regs_ && list.watched && !record_split_;
  if (use_plan && !list.plan_valid) BuildPlan(c, list, words);
  const std::vector<Op>& ops = use_plan ? list.plan : list.ops;
  const uint32_t* plan_values = list.plan_values.data();

  if (d3d_list_mode_ == d3d_draw::kSliceVerify) {
    // Model the run; the decoder then runs it and D3dListVerify* compare.
    if (d3d_list_verify_depth_ != ~0u) return false;  // nested: one at a time
    D3dListCompiler::Model& m = c.model;
    if (!m.values) {
      m.values.reset(new uint32_t[RegisterFile::kRegisterCount]);
      m.stamp.reset(new uint32_t[RegisterFile::kRegisterCount]());
    }
    if (++m.epoch == 0) {
      std::memset(m.stamp.get(), 0, RegisterFile::kRegisterCount * sizeof(uint32_t));
      m.epoch = 1;
    }
    m.touched.clear();
    m.draws.clear();
    m.loads.clear();
    m.seen_loads.clear();
    m.modelled_draws = 0;
    m.seen_draws = 0;
    m.mismatches = 0;
    m.opaque = false;
    m.key = key;
    if (use_plan) {
      ++c.stats.plan_runs;
      c.stats.plan_registers += list.plan_written;
      c.stats.plan_elided += list.plan_elided;
    }
    // The model evaluates predicates with the bin state the run would have:
    // the list's own bin packets change the decoder's for the walk, which
    // is restored after it.
    const uint64_t saved_mask = bin_mask_, saved_select = bin_select_;
    for (const Op& op : ops) {
      if (m.opaque) break;
      switch (op.kind) {
        case D3dListCompiler::kOpRegs:
          if (D3dListCompiler::Predicated(op.packet) &&
              PredicatedOut(op.packet, (op.packet >> 8) & 0x7F)) {
            break;
          }
          for (uint32_t i = 0; i < op.count; ++i) {
            c.ModelWrite(op.first + i, Swap(words[op.payload + i]));
          }
          break;
        case D3dListCompiler::kOpHost:
          for (uint32_t i = 0; i < op.count; ++i) {
            c.ModelWrite(op.first + i, plan_values[op.payload + i]);
          }
          break;
        case D3dListCompiler::kOpRegsMem: {
          if (D3dListCompiler::Predicated(op.packet) &&
              PredicatedOut(op.packet, (op.packet >> 8) & 0x7F)) {
            break;
          }
          const uint32_t* values = memory_->TranslatePhysical<const uint32_t*>(op.payload);
          for (uint32_t i = 0; i < op.count; ++i) c.ModelWrite(op.first + i, Swap(values[i]));
        } break;
        case D3dListCompiler::kOpRegsOne:
          for (uint32_t i = 0; i < op.count; ++i) {
            c.ModelWrite(op.first, Swap(words[op.payload + i]));
          }
          break;
        case D3dListCompiler::kOpShaderGuest:
          m.loads.push_back({op.first, op.payload, op.count});
          break;
        case D3dListCompiler::kOpShaderInline:
          m.loads.push_back({op.first, 0, op.count});
          break;
        case D3dListCompiler::kOpDraw: {
          auto& checkpoint = m.draws.emplace_back();
          checkpoint.reserve(m.touched.size());
          for (uint32_t index : m.touched) checkpoint.emplace_back(index, m.values[index]);
          ++m.modelled_draws;
        } break;
        case D3dListCompiler::kOpBin: {
          const uint64_t value = Swap(words[op.payload]);
          switch (op.first) {
            case PM4_SET_BIN_MASK_LO:
              bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
              break;
            case PM4_SET_BIN_MASK_HI:
              bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (value << 32);
              break;
            case PM4_SET_BIN_SELECT_LO:
              bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
              break;
            case PM4_SET_BIN_SELECT_HI:
              bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (value << 32);
              break;
            default:  // PM4_SET_BIN_MASK: high word, low word
              bin_mask_ = (value << 32) | Swap(words[op.payload + 1]);
              break;
          }
        } break;
        case D3dListCompiler::kOpPacket:
          if (!GenericKeepsRegisters(op.packet)) {
            m.opaque = true;
            ++c.generic[D3dListCompiler::GenericKey(op.packet)];
          }
          break;
      }
    }
    bin_mask_ = saved_mask;
    bin_select_ = saved_select;
    d3d_list_verify_depth_ = trace_ib_depth_;
    return false;
  }

  // Replay.
  ++c.stats.replayed;
  c.stats.ops += ops.size();
  c.stats.draws += list.draws;
  if (use_plan) {
    ++c.stats.plan_runs;
    c.stats.plan_registers += list.plan_written;
    c.stats.plan_elided += list.plan_elided;
  }
  for (size_t index = 0; index < ops.size(); ++index) {
    const Op& op = ops[index];
    switch (op.kind) {
      case D3dListCompiler::kOpHost:
        WriteRegistersHost(op.first, plan_values + op.payload, op.count);
        break;
      case D3dListCompiler::kOpRegs:
        if (D3dListCompiler::Predicated(op.packet) &&
            PredicatedOut(op.packet, (op.packet >> 8) & 0x7F)) {
          break;
        }
        PacketWriteRegistersFromMem(op.first, words + op.payload, op.count);
        break;
      case D3dListCompiler::kOpRegsMem:
        if (D3dListCompiler::Predicated(op.packet) &&
            PredicatedOut(op.packet, (op.packet >> 8) & 0x7F)) {
          break;
        }
        PacketWriteRegistersFromMem(op.first, memory_->TranslatePhysical<uint32_t*>(op.payload),
                                    op.count);
        break;
      case D3dListCompiler::kOpRegsOne:
        for (uint32_t i = 0; i < op.count; ++i) {
          PacketWriteRegister(op.first, Swap(words[op.payload + i]));
        }
        break;
      case D3dListCompiler::kOpShaderGuest:
        LoadGuestShader(xenos::ShaderType(op.first), op.payload, op.count);
        break;
      case D3dListCompiler::kOpShaderInline: {
        const uint32_t* ucode = words + op.payload;
        LoadInlineShader(xenos::ShaderType(op.first), uint32_t(uintptr_t(ucode)), ucode,
                         op.count);
      } break;
      case D3dListCompiler::kOpDraw: {
        // DL-2.2 statistics: replayed draws by tiling pass (-1 untiled, 0
        // the first tile, 1 and up a later one), those the bin predicate
        // culled, and those the backend dropped for the untile pass.
        const int32_t pass = TilingPassOfSelect();
        const uint32_t tile = pass < 0 ? 0 : (pass == 0 ? 1 : 2);
        ++c.stats.tile_draws[tile];
        if ((op.packet & 1) && PredicatedOut(op.packet, op.first)) {
          ++c.stats.tile_culled[tile];
          break;
        }
        const uint64_t untile_before = untile_skipped_draws_;
        memory::RingBuffer reader(
            reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(words + op.payload)),
            op.count * sizeof(uint32_t));
        reader.set_write_offset(op.count * sizeof(uint32_t));
        if (op.first == PM4_DRAW_INDX) {
          ExecutePacketType3_DRAW_INDX(&reader, op.packet, op.count);
        } else {
          ExecutePacketType3_DRAW_INDX_2(&reader, op.packet, op.count);
        }
        c.stats.tile_untile_skipped[tile] += untile_skipped_draws_ - untile_before;
      } break;
      case D3dListCompiler::kOpBin: {
        const uint64_t value = Swap(words[op.payload]);
        switch (op.first) {
          case PM4_SET_BIN_MASK_LO:
            bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
            break;
          case PM4_SET_BIN_MASK_HI:
            bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (value << 32);
            break;
          case PM4_SET_BIN_SELECT_LO:
            bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
            break;
          case PM4_SET_BIN_SELECT_HI:
            bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (value << 32);
            break;
          default:  // PM4_SET_BIN_MASK: high word, low word
            bin_mask_ = (value << 32) | Swap(words[op.payload + 1]);
            break;
        }
      } break;
      case D3dListCompiler::kOpPacket: {
        ++c.stats.generic_ops;
        ++c.generic[D3dListCompiler::GenericKey(op.packet)];
        const uint32_t size =
            (op.packet >> 30) == 1 ? 3 : 2 + ((op.packet >> 16) & 0x3FFF);
        memory::RingBuffer reader(
            reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(words + op.offset)),
            size * sizeof(uint32_t));
        reader.set_write_offset(size * sizeof(uint32_t));
        if (!ExecutePacket(&reader)) {
          // As the decoder: a bad packet ends the buffer.
          REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
          return true;
        }
        if (d3d_draw_open_) {
          // A marker opened a DL-1.2 window: decode the rest.
          ++c.stats.bailed;
          ExecutePacketsFrom(words, count, op.offset + size);
          return true;
        }
      } break;
    }
  }
  return true;
}

void CommandProcessor::D3dListVerifyDraw() {
  D3dListCompiler& c = *d3d_lists_;
  D3dListCompiler::Model& m = c.model;
  const uint32_t draw = m.seen_draws++;
  if (draw >= m.modelled_draws || draw >= m.draws.size()) return;
  ++c.stats.verified_draws;
  const RegisterFile& regs = decode_regs();
  for (const auto& [index, value] : m.draws[draw]) {
    if (IsPerDrawRegister(index)) continue;
    ++c.stats.verified_registers;
    if (regs.values[index] != value) {
      ++m.mismatches;
      ++c.stats.register_mismatches;
      if (c.logs < 64) {
        ++c.logs;
        REXGPU_INFO("DL-2.1 list verify: list {:08X}+{} draw {} register {:04X} modelled {:08X} "
                    "decoded {:08X}",
                    uint32_t(m.key >> 32), uint32_t(m.key), draw, index, value,
                    regs.values[index]);
      }
    }
  }
}

void CommandProcessor::D3dListVerifyLoad(uint32_t type, uint32_t address, uint32_t dwords) {
  d3d_lists_->model.seen_loads.push_back({type, address, dwords});
}

void CommandProcessor::D3dListVerifyEnd() {
  D3dListCompiler& c = *d3d_lists_;
  D3dListCompiler::Model& m = c.model;
  d3d_list_verify_depth_ = ~0u;
  ++c.stats.verified;
  if (m.opaque) ++c.stats.opaque;
  const RegisterFile& regs = decode_regs();
  if (!m.opaque) {
    for (uint32_t index : m.touched) {
      if (IsPerDrawRegister(index)) continue;
      ++c.stats.verified_registers;
      if (regs.values[index] != m.values[index]) {
        ++m.mismatches;
        ++c.stats.register_mismatches;
        if (c.logs < 64) {
          ++c.logs;
          REXGPU_INFO("DL-2.1 list verify: list {:08X}+{} end register {:04X} modelled {:08X} "
                      "decoded {:08X}",
                      uint32_t(m.key >> 32), uint32_t(m.key), index, m.values[index],
                      regs.values[index]);
        }
      }
    }
    if (m.seen_draws != m.modelled_draws) {
      ++c.stats.draw_count_mismatches;
      ++m.mismatches;
      if (c.logs < 64) {
        ++c.logs;
        REXGPU_INFO("DL-2.1 list verify: list {:08X}+{} modelled {} draws, decoded {}",
                    uint32_t(m.key >> 32), uint32_t(m.key), m.modelled_draws, m.seen_draws);
      }
    }
  }
  // Loads: the modelled ones must open the decoded sequence.
  bool loads_match = m.seen_loads.size() >= m.loads.size() &&
                     (m.opaque || m.seen_loads.size() == m.loads.size());
  for (size_t i = 0; loads_match && i < m.loads.size(); ++i) {
    const auto& a = m.loads[i];
    const auto& b = m.seen_loads[i];
    loads_match = a.type == b.type && a.dwords == b.dwords && (!a.address || a.address == b.address);
  }
  if (!loads_match) {
    ++c.stats.load_mismatches;
    ++m.mismatches;
    if (c.logs < 64) {
      ++c.logs;
      REXGPU_INFO("DL-2.1 list verify: list {:08X}+{} modelled {} shader loads, decoded {}",
                  uint32_t(m.key >> 32), uint32_t(m.key), m.loads.size(), m.seen_loads.size());
    }
  }
  if (m.mismatches) ++c.stats.mismatch_lists;
}

void CommandProcessor::D3dListSwap() {
  int32_t mode = REXCVAR_GET(gpu_d3d_list_compile);
  if (mode < 0) mode = int32_t(d3d_draw::GetSliceMode());
  // The compiled path assumes the decoder's per-packet hooks are idle.
  if (mode && (d3d_census_decoder::Active() || packet_recorder_ ||
               REXCVAR_GET(gpu_trace_bins_vblank) >= 0 || REXCVAR_GET(gpu_ib_identity_stats) ||
               REXCVAR_GET(gpu_template_stats) || REXCVAR_GET(gpu_replay_floor_probe))) {
    mode = 0;
  }
  d3d_list_mode_ = uint32_t(mode);
  int32_t watch = REXCVAR_GET(gpu_d3d_list_watch);
  if (watch < 0) watch = 1;
  int32_t host_regs = REXCVAR_GET(gpu_d3d_list_host_regs);
  if (host_regs < 0) host_regs = 1;
  d3d_list_watch_ = mode && watch;
  d3d_list_host_regs_ = d3d_list_watch_ && host_regs;
  if (mode && !d3d_lists_) d3d_lists_ = std::make_unique<D3dListCompiler>();
  if (!d3d_lists_) return;
  D3dListCompiler& c = *d3d_lists_;
  if (d3d_list_watch_) D3dPageWatchGet();
  ++c.swaps;
  if (++c.sweep_swaps < D3dListCompiler::kSweepSwaps) return;
  c.sweep_swaps = 0;
  for (auto it = c.lists.begin(); it != c.lists.end();) {
    it->second.changes = 0;
    it->second.dirty = 0;
    if (c.swaps - it->second.last_swap > D3dListCompiler::kSweepSwaps) {
      it = c.lists.erase(it);
      ++c.stats.evicted;
    } else {
      ++it;
    }
  }
  const auto& s = c.stats;
  if (s.runs) {
    std::string fallback;
    for (uint32_t i = 0; i < D3dListCompiler::kFallbackCount; ++i) {
      if (s.fallback[i]) {
        fallback += fmt::format(" {} {} ({} draws),", D3dListCompiler::kFallbackName[i],
                                s.fallback[i], s.fallback_draws[i]);
      }
    }
    REXGPU_INFO(
        "DL-2.1 lists ({} swaps, mode {}): {} nested runs, {} replayed ({} draws, {} ops, {} "
        "generic, {} bailed to decode), {} compiles, {} lists held, {} evicted; decoded:{}",
        D3dListCompiler::kSweepSwaps, d3d_list_mode_, s.runs, s.replayed, s.draws, s.ops,
        s.generic_ops, s.bailed, s.compiles, c.lists.size(), s.evicted,
        fallback.empty() ? " none" : fallback);
    uint64_t faults = 0, fault_pages = 0, arms = 0, arm_pages = 0;
    if (d3d_page_watch_) {
      // Arms include the shader cache's.
      D3dPageWatch& w = *d3d_page_watch_;
      faults = w.faults.exchange(0, std::memory_order_relaxed);
      fault_pages = w.fault_pages.exchange(0, std::memory_order_relaxed);
      arms = w.arms;
      arm_pages = w.arm_pages;
      w.arms = w.arm_pages = 0;
    }
    REXGPU_INFO(
        "DL-2.1 list validation (watch {}, plan {}): {} clean by the watch, {} checked ({} "
        "words), {} written since validated, {} put on checks, {} watch misses; watch: {} "
        "arms ({} pages), {} writes dropped a watch ({} pages); plan: {} runs, {} builds, {} "
        "registers written, {} dropped",
        d3d_list_watch_, d3d_list_host_regs_, s.clean, s.checked, s.check_words, s.dirty,
        s.watch_off, s.watch_misses, arms, arm_pages, faults, fault_pages, s.plan_runs,
        s.plan_builds, s.plan_registers, s.plan_elided);
    if (s.tile_draws[1] || s.tile_draws[2]) {
      REXGPU_INFO(
          "DL-2.2 list draws by tiling pass (replayed / culled by the bin predicate / dropped "
          "for the untile pass): untiled {} / {} / {}, first tile {} / {} / {}, later tiles {} / "
          "{} / {}",
          s.tile_draws[0], s.tile_culled[0], s.tile_untile_skipped[0], s.tile_draws[1],
          s.tile_culled[1], s.tile_untile_skipped[1], s.tile_draws[2], s.tile_culled[2],
          s.tile_untile_skipped[2]);
    }
    if (s.verified) {
      REXGPU_INFO(
          "DL-2.1 list verify: {} runs ({} unmodelled after a generic packet), {} draws, {} "
          "registers checked; {} runs mismatching: {} registers, {} draw counts, {} load "
          "sequences",
          s.verified, s.opaque, s.verified_draws, s.verified_registers, s.mismatch_lists,
          s.register_mismatches, s.draw_count_mismatches, s.load_mismatches);
    }
  }
  std::string top;
  for (uint32_t key = 0; key < std::size(c.generic); ++key) {
    const uint64_t n = c.generic[key];
    if (!n) continue;
    top += key == 0x200 ? fmt::format(" type1x{}", n)
                        : fmt::format(" {}{:02X}x{}", key & 0x100 ? "p" : "", key & 0xFF, n);
    c.generic[key] = 0;
  }
  if (!top.empty()) REXGPU_INFO("DL-2.1 lists: generic packets by opcode:{}", top);
  c.stats = {};
}

}  // namespace rex::graphics
