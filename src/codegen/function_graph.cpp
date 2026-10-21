/**
 * @file        rexcodegen/function_graph.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"
#include "ppc/disasm.h"
#include "builders/builder_context.h"
#include "builders.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <atomic>
#include <iterator>
#include <cassert>
#include <cstring>
#include <optional>
#include <set>
#include <cctype>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/code_emitter.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/function_scanner.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "codegen_logging.h"

#include <dis-asm.h>
#include <ppc.h>

using rex::codegen::ppc::Disassemble;
using rex::memory::load_and_swap;

namespace rex::codegen {

//=============================================================================
// Utility
//=============================================================================

const char* AuthorityName(FunctionAuthority auth) {
  switch (auth) {
    case FunctionAuthority::GAP_FILL:
      return "gap_fill";
    case FunctionAuthority::DISCOVERED:
      return "discovered";
    case FunctionAuthority::VTABLE:
      return "vtable";
    case FunctionAuthority::HELPER:
      return "helper";
    case FunctionAuthority::PDATA:
      return "pdata";
    case FunctionAuthority::CONFIG:
      return "config";
    case FunctionAuthority::IMPORT:
      return "import";
    default:
      return "unknown";
  }
}

//=============================================================================
// FunctionNode
//=============================================================================

FunctionNode::FunctionNode(uint32_t base, uint32_t size, FunctionAuthority authority)
    : base_(base), size_(size), authority_(authority), state_(FunctionState::kRegistered) {
  // Generate default name
  char buf[32];
  snprintf(buf, sizeof(buf), "sub_%08X", base);
  name_ = buf;

  // All functions start kRegistered - waiting for discover() to assign blocks.
  // Authority prevents merging via vacancy checks, not via initial state.
}

//=============================================================================
// State Machine Methods
//=============================================================================

void FunctionNode::discover(std::vector<Block> blocks,
                            std::vector<rex::codegen::ppc::Instruction*> instructions,
                            std::set<uint32_t> labels) {
  assert(canDiscover() && "Invalid state transition: must be kRegistered");

  // Non-imports must have blocks
  if (!isImport()) {
    assert(!blocks.empty() && "Non-import function must have blocks");
  }

  blocks_ = std::move(blocks);
  instructions_ = std::move(instructions);
  labels_ = std::move(labels);

  // Update size based on blocks
  for (const auto& block : blocks_) {
    uint32_t blockEnd = block.base + block.size;
    if (blockEnd > base_ + size_) {
      size_ = blockEnd - base_;
    }
  }

  state_ = FunctionState::kDiscovered;
  REXCODEGEN_DEBUG(
      "FunctionNode 0x{:08X} ({}): DISCOVERED with {} blocks, {} instructions, {} labels", base_,
      name_, blocks_.size(), instructions_.size(), labels_.size());
}

void FunctionNode::discoverAsImport() {
  assert(canDiscover() && "Invalid state transition: must be kRegistered");
  assert(isImport() && "Only imports can use discoverAsImport()");

  // blocks_ and instructions_ remain empty for imports
  state_ = FunctionState::kDiscovered;
  REXCODEGEN_DEBUG("FunctionNode 0x{:08X} ({}): DISCOVERED as import", base_, name_);
}

bool FunctionNode::canSeal() const {
  if (state_ != FunctionState::kDiscovered)
    return false;
  if (isImport())
    return true;  // Imports can seal without blocks
  if (blocks_.empty())
    return false;  // Non-imports must have blocks
  if (!unresolvedJumps_.empty())
    return false;  // All branches must be resolved
  return true;
}

const FunctionAnalysis& FunctionNode::analysis() const {
  assert(state_ == FunctionState::kSealed && "Analysis only valid after seal");
  assert(analysis_.has_value() && "Analysis not computed");
  return *analysis_;
}

void FunctionNode::addBlock(Block block) {
  blocks_.push_back(block);

  // Extend size if block extends past current end
  uint32_t blockEnd = block.base + block.size;
  if (blockEnd > base_ + size_) {
    size_ = blockEnd - base_;
  }
}

bool FunctionNode::containsAddress(uint32_t addr) const {
  // First check overall bounds
  if (addr < base_ || addr >= base_ + size_) {
    return false;
  }

  // If no blocks defined, use linear range
  if (blocks_.empty()) {
    return true;
  }

  // Check individual blocks
  for (const auto& block : blocks_) {
    if (block.contains(addr)) {
      return true;
    }
  }

  // For CONFIG and PDATA functions, trust the declared size even if blocks don't cover it
  // This handles out-of-line switch cases where compiler places code after epilogue
  if (authority_ == FunctionAuthority::CONFIG || authority_ == FunctionAuthority::PDATA) {
    return true;  // Already passed bounds check above
  }

  return false;
}

void FunctionNode::addLabel(uint32_t addr) {
  labels_.insert(addr);
}

void FunctionNode::addCall(uint32_t site, CallTarget target) {
  calls_.push_back({site, std::move(target)});
}

void FunctionNode::addTailCall(uint32_t site, CallTarget target) {
  tailCalls_.push_back({site, std::move(target)});
}

void FunctionNode::addJumpTable(JumpTable jt) {
  // All jump table targets become labels
  for (uint32_t target : jt.targets) {
    labels_.insert(target);
  }
  jumpTables_.push_back(std::move(jt));
}

void FunctionNode::addUnresolvedJump(uint32_t site, uint32_t target, bool isCall,
                                     bool conditional) {
  unresolvedJumps_.push_back({site, target, isCall, conditional});
}

void FunctionNode::removeUnresolvedJump(uint32_t site) {
  auto it = std::remove_if(unresolvedJumps_.begin(), unresolvedJumps_.end(),
                           [site](const UnresolvedJump& j) { return j.site == site; });
  unresolvedJumps_.erase(it, unresolvedJumps_.end());
}

bool FunctionNode::tryResolveAgainst(FunctionNode* newFunction) {
  if (isSealed())
    return false;
  if (!newFunction)
    return false;

  bool anyResolved = false;

  // Check each unresolved jump
  for (auto it = unresolvedJumps_.begin(); it != unresolvedJumps_.end();) {
    if (it->target == newFunction->base()) {
      // Target matches new function's entry point -> external call/tail call
      REXCODEGEN_TRACE(
          "FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as external to {}", base_,
          it->site, it->target, newFunction->name());

      // Add as tail call (unconditional branch to another function)
      addTailCall(it->site, CallTarget::function(newFunction));

      it = unresolvedJumps_.erase(it);
      anyResolved = true;
    } else {
      ++it;
    }
  }

  return anyResolved;
}

bool FunctionNode::tryResolveAgainstImport(uint32_t importAddr, const std::string& importName) {
  if (isSealed())
    return false;

  bool anyResolved = false;

  for (auto it = unresolvedJumps_.begin(); it != unresolvedJumps_.end();) {
    if (it->target == importAddr) {
      REXCODEGEN_TRACE("FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as import {}",
                       base_, it->site, it->target, importName);

      // Add as tail call to import
      addTailCall(it->site, CallTarget::import(importAddr, importName));

      it = unresolvedJumps_.erase(it);
      anyResolved = true;
    } else {
      ++it;
    }
  }

  return anyResolved;
}

bool FunctionNode::tryResolveAsInternalLabel(uint32_t target) {
  // Check if target is within our blocks
  if (!containsAddress(target)) {
    return false;
  }

  // It's internal - add as label
  addLabel(target);
  return true;
}

void FunctionNode::absorbRegion(uint32_t regionBase, uint32_t regionSize) {
  assert(state_ != FunctionState::kSealed && "Cannot absorb into SEALED function");

  // Add as a new block
  addBlock({regionBase, regionSize});

  REXCODEGEN_DEBUG("FunctionNode 0x{:08X}: absorbed region 0x{:08X}-0x{:08X}, new size=0x{:X}",
                   base_, regionBase, regionBase + regionSize, size_);
}

void FunctionNode::seal() {
  assert(canSeal() && "Cannot seal: invariants not met");

  // Sort blocks by address for correct emission order
  std::sort(blocks_.begin(), blocks_.end(),
            [](const Block& a, const Block& b) { return a.base < b.base; });

  // Merge overlapping blocks - can happen when multiple paths reach the same code
  // (e.g., jump table targets and unconditional branches both targeting an epilogue)
  if (blocks_.size() > 1) {
    std::vector<Block> merged;
    merged.reserve(blocks_.size());
    merged.push_back(blocks_[0]);

    for (size_t i = 1; i < blocks_.size(); i++) {
      Block& last = merged.back();
      const Block& curr = blocks_[i];

      // Check for overlap or adjacency
      if (curr.base <= last.end()) {
        // Extend last block to cover both
        uint32_t newEnd = std::max(last.end(), curr.end());
        last.size = newEnd - last.base;
        REXCODEGEN_TRACE(
            "FunctionNode 0x{:08X}: merged overlapping blocks 0x{:08X}-0x{:08X} and "
            "0x{:08X}-0x{:08X}",
            base_, last.base, last.base + last.size - (newEnd - last.end()), curr.base, curr.end());
      } else {
        merged.push_back(curr);
      }
    }

    if (merged.size() < blocks_.size()) {
      REXCODEGEN_DEBUG("FunctionNode 0x{:08X}: reduced {} blocks to {} after merging", base_,
                       blocks_.size(), merged.size());
      blocks_ = std::move(merged);
    }
  }

  // Compute function analysis
  FunctionAnalysis analysis;
  analysis_ = std::move(analysis);
  state_ = FunctionState::kSealed;

  REXCODEGEN_TRACE(
      "FunctionNode 0x{:08X} ({}): SEALED with {} blocks, {} labels, {} calls, {} tail calls",
      base_, name_, blocks_.size(), labels_.size(), calls_.size(), tailCalls_.size());
}

//=============================================================================
// FunctionNode - C++ Code Emission
//=============================================================================

namespace {

// Helper: append formatted text to a raw string (matches Recompiler::println pattern)
template <class... Args>
void emit_println(std::string& out, fmt::format_string<Args...> fmt, Args&&... args) {
  fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
  out += '\n';
}

template <class... Args>
void emit_print(std::string& out, fmt::format_string<Args...> fmt, Args&&... args) {
  fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
}

// The builders track the MXCSR/FPCR flush mode only along straight-line code:
// every label resets it to unknown, so the first floating-point instruction
// after any label emits a conditional toggle (a load, a compare and a branch,
// and in a loop that alternates scalar and vector work a mode write too).
// This pass computes the mode at every label as the meet of the paths into it
// (fall-through and every goto, iterated to a fixed point for loops) and drops
// each conditional toggle whose incoming mode is already the one it sets.
// Entry, labels the resume dispatch can enter, calls (the callee may leave
// either mode) and anything the pass does not recognize count as unknown, so
// a toggle is removed only where every path provably sets that mode first.
std::string PropagateFlushMode(const std::string& body) {
  enum State : uint8_t { kTop, kFpu, kVmx, kUnknown };
  auto meet = [](State a, State b) {
    if (a == kTop)
      return b;
    if (b == kTop || a == b)
      return a;
    return kUnknown;
  };

  std::vector<std::string_view> lines;
  for (size_t pos = 0; pos < body.size();) {
    size_t end = body.find('\n', pos);
    if (end == std::string::npos)
      end = body.size();
    lines.emplace_back(body.data() + pos, end - pos);
    pos = end + 1;
  }

  auto contains = [](std::string_view line, std::string_view what) {
    return line.find(what) != std::string_view::npos;
  };
  // The label a line references with `loc_`, or empty.
  auto label_ref = [](std::string_view line) -> std::string_view {
    size_t at = line.find("loc_");
    if (at == std::string_view::npos)
      return {};
    size_t end = at + 4;
    while (end < line.size() && std::isxdigit(static_cast<unsigned char>(line[end])))
      end++;
    return line.substr(at, end - at);
  };

  enum class Kind : uint8_t { kOther, kLabel, kGoto, kJump, kReturn, kBarrier, kToggle, kSet };
  struct Line {
    Kind kind = Kind::kOther;
    State target = kTop;  // for toggles
    std::string_view label;
  };
  std::vector<Line> info(lines.size());
  std::unordered_map<std::string_view, State> label_in;
  for (size_t i = 0; i < lines.size(); i++) {
    std::string_view line = lines[i];
    Line& li = info[i];
    if (line.starts_with("loc_") && line.ends_with(":")) {
      li.kind = Kind::kLabel;
      li.label = line.substr(0, line.size() - 1);
      label_in.emplace(li.label, kTop);
      continue;
    }
    std::string_view code = line.substr(std::min(line.find_first_not_of('\t'), line.size()));
    if (code.starts_with("//"))
      continue;
    // Only top-level toggles are tracked: one nested in a block may not run,
    // so it falls through to the barrier case below.
    const bool top_level = code.data() == line.data() + 1;
    if (top_level &&
        (code == "ctx.fpscr.enableFlushMode();" || code == "ctx.fpscr.disableFlushMode();")) {
      li.kind = Kind::kToggle;
      li.target = code.starts_with("ctx.fpscr.enable") ? kVmx : kFpu;
      continue;
    }
    if (top_level && (code == "ctx.fpscr.enableFlushModeUnconditional();" ||
                      code == "ctx.fpscr.disableFlushModeUnconditional();")) {
      li.kind = Kind::kSet;
      li.target = code.starts_with("ctx.fpscr.enable") ? kVmx : kFpu;
      continue;
    }
    if (contains(code, "loc_")) {
      li.label = label_ref(code);
      // A dispatch case enters with whatever mode the caller left.
      li.kind = contains(code, "case ") ? Kind::kJump : Kind::kGoto;
      if (li.kind == Kind::kGoto && !code.starts_with("goto ") && !code.starts_with("if ("))
        li.kind = Kind::kJump;
      continue;
    }
    if (line == "\treturn;") {
      li.kind = Kind::kReturn;
      continue;
    }
    if (contains(code, "(ctx, base") || contains(code, "REX_CALL") ||
        contains(code, "REX_TAIL_CALL") || contains(code, "ctx = ")|| contains(code, "SEH") || contains(code, "jmp") ||
        contains(code, "fpscr")) {
      li.kind = Kind::kBarrier;
    }
  }

  std::vector<State> toggle_in(lines.size(), kTop);
  for (bool changed = true; changed;) {
    changed = false;
    State state = kUnknown;  // function entry
    for (size_t i = 0; i < lines.size(); i++) {
      const Line& li = info[i];
      switch (li.kind) {
        case Kind::kLabel:
          state = meet(state, label_in[li.label]);
          break;
        case Kind::kGoto:
        case Kind::kJump: {
          State edge = li.kind == Kind::kJump ? kUnknown : state;
          State& in = label_in[li.label];
          State next = meet(in, edge);
          if (next != in) {
            in = next;
            changed = true;
          }
          // Only a top-level unconditional goto ends the fall-through.
          if (lines[i].starts_with("\tgoto "))
            state = kTop;
          break;
        }
        case Kind::kReturn:
          state = kTop;
          break;
        case Kind::kBarrier:
          state = kUnknown;
          break;
        case Kind::kToggle:
          toggle_in[i] = state;
          state = li.target;
          break;
        case Kind::kSet:
          state = li.target;
          break;
        case Kind::kOther:
          break;
      }
    }
  }

  std::string out;
  out.reserve(body.size());
  for (size_t i = 0; i < lines.size(); i++) {
    if (info[i].kind == Kind::kToggle && toggle_in[i] == info[i].target)
      continue;
    out.append(lines[i]);
    if (i + 1 < lines.size() || (!body.empty() && body.back() == '\n'))
      out += '\n';
  }
  return out;
}

// With cr_as_local, a function keeps the CR fields it uses in locals, which the
// compiler can hold in host registers: as ctx members they are bytes that every
// guest store may alias. ctx stays the state everything outside the function
// sees: the locals are loaded from ctx on entry (before the resume switch, so
// an interior re-entry starts from ctx too), written back before every line
// that can leave the function or hand ctx to other code (calls, returns, tail
// calls, longjmp, the setjmp context copy, hooks given ctx), and reloaded after
// the ones that come back (and at the start of the SEH catch block, where ctx
// holds the state the exception left).
//
// Calls, returns and tail calls follow the PowerPC ABI: cr2-cr4 are
// non-volatile, so a callee preserves them and never reads cr0, cr1 or
// cr5-cr7 on entry, and a caller never reads those after a call. Those lines
// write back only the non-volatile fields (`spill_nv`) and reload nothing:
// ctx.cr2-cr4 come back unchanged, and the volatile fields are dead. Hooks,
// the setjmp copy, longjmp and exceptions keep the full synchronization.
std::string SyncLocalCrRegisters(const std::string& body, const std::string& spill,
                                 const std::string& spill_nv, const std::string& reload,
                                 const std::string& reload_after_call = {}) {
  auto contains = [](std::string_view line, std::string_view what) {
    return line.find(what) != std::string_view::npos;
  };
  std::string out;
  out.reserve(body.size() + body.size() / 4);
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    if (end == std::string::npos)
      end = body.size();
    std::string_view line(body.data() + pos, end - pos);
    pos = end + 1;

    size_t first = line.find_first_not_of('\t');
    std::string_view code =
        first == std::string_view::npos ? std::string_view{} : line.substr(first);
    auto emit = [&out](std::string_view text) {
      out.append(text);
      out += '\n';
    };

    if (code.empty() || code.starts_with("//")) {
      emit(line);
      continue;
    }
    // A hook that takes ctx and decides a branch: `if (Hook(ctx, base)) {`.
    if (code.starts_with("if (") && code.ends_with(") {") && contains(code, "ctx, base")) {
      std::string_view condition = code.substr(4, code.size() - 4 - 3);
      out += spill;
      out += '\n';
      out.append(line.substr(0, first));
      out += "if ([&] { const bool rex_hook_result = (";
      out.append(condition);
      out += ");";
      out += reload;
      out += " return rex_hook_result; }()) {\n";
      continue;
    }
    if (contains(code, "env = ctx;") && !contains(code, "ctx = env;")) {
      out += spill;
      out += '\n';
      emit(line);
      continue;
    }
    if (contains(code, "ctx = env;")) {
      emit(line);
      out += reload;
      out += '\n';
      continue;
    }
    if (contains(code, "SEH_CATCH_ALL")) {
      emit(line);
      out += reload;
      out += '\n';
      continue;
    }
    if (contains(code, "ppc_longjmp") || contains(code, "SEH_RETHROW")) {
      out += spill;
      out += '\n';
      emit(line);
      continue;
    }
    const bool leaves = contains(code, "return") || contains(code, "REX_TAIL_CALL");
    const bool calls = contains(code, "ctx, base") || contains(code, "REX_CALL_INDIRECT_FUNC");
    if (leaves || calls) {
      if (!spill_nv.empty()) {
        out += spill_nv;
        out += '\n';
      }
      emit(line);
      if (calls && !leaves && !reload_after_call.empty()) {
        out += reload_after_call;
        out += '\n';
      }
      continue;
    }
    emit(line);
  }
  return out;
}

// The non-volatile register locals (r14-r31, f14-f31, v14-v31, v64-v127) a
// function body assigns: `rN.u64 = `, `vN.u8[i] = `, or a vector store whose
// destination is the local (`simde_mm_store_*((...*)vN.`).
std::vector<std::string> WrittenLocalRegisters(const std::string& body) {
  std::set<std::string> written;
  auto isNonVolatile = [](char kind, unsigned index) {
    if (kind == 'r' || kind == 'f')
      return index >= 14 && index < 32;
    return (index >= 14 && index < 32) || (index >= 64 && index < 128);
  };
  auto parseRegister = [&](std::string_view text, size_t at, std::string& out) {
    if (at >= text.size())
      return false;
    char kind = text[at];
    if (kind != 'r' && kind != 'f' && kind != 'v')
      return false;
    size_t q = at + 1;
    while (q < text.size() && std::isdigit(static_cast<unsigned char>(text[q])))
      q++;
    if (q == at + 1 || q >= text.size() || (text[q] != '.' && text[q] != '[' && text[q] != ' '))
      return false;
    unsigned index = static_cast<unsigned>(std::stoul(std::string(text.substr(at + 1, q - at - 1))));
    if (!isNonVolatile(kind, index))
      return false;
    out = std::string(text.substr(at, q - at));
    return true;
  };
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    if (end == std::string::npos)
      end = body.size();
    std::string_view line(body.data() + pos, end - pos);
    pos = end + 1;
    size_t first = line.find_first_not_of('\t');
    if (first == std::string_view::npos)
      continue;
    std::string_view code = line.substr(first);
    if (code.ends_with("// nv-helper"))
      continue;  // an inlined save or restore: the local only gets its entry value back
    std::string reg;
    if (code.starts_with("simde_mm_store")) {
      size_t open = code.find('(');
      size_t comma = code.find(',');
      if (open != std::string_view::npos && comma != std::string_view::npos) {
        // the destination operand ends at the first comma: `(...*)vN.` or `vN.`
        size_t cast = code.rfind(')', comma);
        size_t start = (cast != std::string_view::npos && cast > open) ? cast + 1 : open + 1;
        if (parseRegister(code, start, reg))
          written.insert(reg);
      }
      continue;
    }
    if (!parseRegister(code, 0, reg))
      continue;
    size_t eq = code.find(" = ");
    size_t paren = code.find('(');
    if (eq != std::string_view::npos && (paren == std::string_view::npos || eq < paren))
      written.insert(reg);
  }
  return std::vector<std::string>(written.begin(), written.end());
}

//=============================================================================
// host_local_register_saves: the save-area scan
//=============================================================================
// A function whose prologue calls __savegprlr_N may keep rN-r31 in host locals
// (BuilderContext::emitHostLocalSaveRestore) only if nothing else reads or
// writes the GPR save slots [entry_sp - 8 - 8 * (32 - N), entry_sp - 8): its
// own code (an inlined epilogue's loads, a slot read, a pointer into the
// area), an unwinder (exception info), a hook, or a path that skips the save.
// The scan follows r1 and the registers derived from it in address order (the
// tracking restarts at every label) and refuses anything it cannot place.

enum class HostSaveReason : uint8_t {
  kTransformed,
  kNoSave,
  kKeepList,
  kLocalized,
  kInteriorResume,
  kSharesRegisters,
  kExceptionInfo,
  kMidAsmHook,
  kMultipleSaves,
  kSaveNotAtEntry,
  kRestoreWider,
  kRestoreFrame,
  kSetJmp,
  kTailBranch,
  kIndirectBranch,
  kFallthrough,
  kDynamicFrame,
  kR1Write,
  kSaveSlotAccess,
  kSaveAreaAddress,
  kSpStored,
  kSpArithmetic,
  kSpIndexedUnknown,
  kUndecoded,
  kCount,
};

constexpr const char* kHostSaveReasonNames[] = {
    "transformed",      "no_save",           "keep_list",          "non_volatile_as_local",
    "interior_resume",  "shares_registers",  "exception_info",     "mid_asm_hook",
    "multiple_saves",   "save_not_at_entry", "restore_wider",      "restore_frame",
    "setjmp_longjmp",   "tail_branch",       "indirect_branch",    "fallthrough",
    "dynamic_frame",    "r1_write",          "save_slot_access",   "save_area_address",
    "sp_stored",        "sp_arithmetic",     "sp_indexed_unknown", "undecoded",
};
static_assert(std::size(kHostSaveReasonNames) == static_cast<size_t>(HostSaveReason::kCount));

std::array<std::atomic<uint64_t>, static_cast<size_t>(HostSaveReason::kCount)> gHostSaveStats{};

struct HostSavePlan {
  HostSaveReason reason = HostSaveReason::kNoSave;
  int first = 0;          ///< first saved GPR when transformed
  uint32_t address = 0;   ///< instruction that decided a refusal
};

// The bytes a load or store touches, by mnemonic (b/h/w/d, f s/d, vectors).
int64_t AccessSize(std::string_view name) {
  if (name.starts_with("lv") || name.starts_with("stv"))
    return 16;
  const std::string_view rest = name.substr(name.starts_with("st") ? 2 : 1);
  if (rest.empty())
    return 8;
  switch (rest[0]) {
    case 'b': return 1;
    case 'h': return 2;
    case 'w': return 4;
    case 'd': return 8;
    case 'f':
      return rest.size() > 1 && (rest[1] == 's' || rest[1] == 'i') ? 4 : 8;
    default: return 8;
  }
}

// Mnemonics whose first operand is not a GPR destination.
bool FirstOperandNotGprDest(std::string_view name) {
  static constexpr std::string_view kPrefixes[] = {
      "st", "cmp", "b", "f", "v", "tw", "td", "mt", "dcb", "icb", "sync", "lwsync", "isync",
      "eieio", "ptesync", "nop", "cr", "mc", "sc", "db", "lf", "lv", "mffs", "mfvscr", "dss",
      "dst", "attn", "trap", "tlb", "slb"};
  for (std::string_view prefix : kPrefixes)
    if (name.starts_with(prefix))
      return true;
  return false;
}

// X-form integer operations whose third operand is a source register.
bool ThirdOperandIsGpr(std::string_view name) {
  while (!name.empty() && (name.back() == '.' || name.back() == 'o'))
    name.remove_suffix(1);
  static constexpr std::string_view kNames[] = {
      "add",  "addc", "adde", "subf", "subfc", "subfe", "and",   "andc", "or",
      "orc",  "xor",  "nor",  "nand", "eqv",   "slw",   "srw",   "sraw", "sld",
      "srd",  "srad", "mullw", "mulhw", "mulhwu", "divw", "divwu", "mulld", "mulhd",
      "mulhdu", "divd", "divdu"};
  return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
}

HostSavePlan ScanHostLocalSaves(const FunctionNode& fn, const EmitContext& ctx) {
  using R = HostSaveReason;
  const auto& cfg = ctx.config;
  struct Word {
    uint32_t address;
    uint32_t raw;
    const uint8_t* data;
  };
  std::vector<Word> words;
  for (const auto& block : fn.blocks()) {
    const uint8_t* data = ctx.binary.translate(block.base);
    if (!data)
      continue;
    for (uint32_t address = block.base; address < block.end(); address += 4) {
      const uint8_t* at = data + (address - block.base);
      words.push_back({address, load_and_swap<uint32_t>(at), at});
    }
  }
  auto calleeName = [&](uint32_t target) -> std::string_view {
    const FunctionNode* callee = ctx.graph.getFunction(target);
    return callee ? std::string_view(callee->name()) : std::string_view{};
  };
  auto branchTarget = [](const Word& w) {
    return PPC_OP(w.raw) == PPC_OP_B ? w.address + PPC_BI(w.raw) : w.address + PPC_BD(w.raw);
  };

  // The prologue's save call.
  size_t saveIndex = 0;
  int first = -1;
  int saves = 0;
  for (size_t i = 0; i < words.size(); ++i) {
    const uint32_t op = PPC_OP(words[i].raw);
    if (op != PPC_OP_B || !PPC_BL(words[i].raw))
      continue;
    const int n = SaveRestoreHelperFirstRegister(calleeName(branchTarget(words[i])), "__savegprlr_");
    if (n < 0)
      continue;
    if (saves++ == 0) {
      saveIndex = i;
      first = n;
    }
  }
  if (saves == 0)
    return {R::kNoSave};
  const uint32_t saveAddress = words[saveIndex].address;
  if (saves > 1)
    return {R::kMultipleSaves, 0, saveAddress};
  if (std::binary_search(cfg.hostLocalRegisterSavesKeep.begin(),
                         cfg.hostLocalRegisterSavesKeep.end(), static_cast<uint32_t>(fn.base())))
    return {R::kKeepList, 0, saveAddress};
  if (cfg.nonVolatileRegistersAsLocalVariables)
    return {R::kLocalized, 0, saveAddress};
  if (cfg.interiorResume)
    return {R::kInteriorResume, 0, saveAddress};
  if (fn.sharesRegisters())
    return {R::kSharesRegisters, 0, saveAddress};
  if (fn.hasExceptionInfo())
    return {R::kExceptionInfo, 0, saveAddress};

  // Labels: internal branch targets and switch cases.
  std::unordered_set<uint32_t> labels;
  std::unordered_set<uint32_t> switchBctrs;
  for (const auto& jt : fn.jumpTables()) {
    switchBctrs.insert(jt.bctrAddress);
    labels.insert(jt.targets.begin(), jt.targets.end());
  }
  for (const Word& w : words) {
    if (cfg.midAsmHooks.count(w.address))
      return {R::kMidAsmHook, 0, w.address};
    const uint32_t op = PPC_OP(w.raw);
    if ((op == PPC_OP_B || op == PPC_OP_BC) && fn.containsAddress(branchTarget(w)) &&
        !(PPC_BL(w.raw) && branchTarget(w) == fn.base()))
      labels.insert(branchTarget(w));
    auto st = cfg.switchTables.find(w.address);
    if (st != cfg.switchTables.end()) {
      switchBctrs.insert(w.address);
      labels.insert(st->second.targets.begin(), st->second.targets.end());
    }
    if (op == PPC_OP_CTR && PPC_XOP(w.raw) == 528 && !PPC_BL(w.raw) &&
        !switchBctrs.count(w.address)) {
      FunctionScanner scanner(ctx.binary);
      if (auto jt = scanner.detect_jump_table(w.address)) {
        switchBctrs.insert(w.address);
        labels.insert(jt->targets.begin(), jt->targets.end());
      }
    }
  }

  // The GPR save area relative to the entry stack pointer; the LR slot at -8
  // stays written and is not part of it.
  const int64_t dangerHi = -8;
  const int64_t dangerLo = dangerHi - 8 * (32 - first);
  auto overlapsArea = [&](int64_t lo, int64_t hi) { return lo < dangerHi && hi > dangerLo; };

  int64_t spOff = 0;  // r1 minus the entry stack pointer
  std::optional<int64_t> bodyOff;
  bool saved = false;
  bool controlFlowSeen = false;  // a label or internal branch since entry
  std::unordered_map<uint32_t, int64_t> spRel;  // GPR -> entry-relative stack address
  std::unordered_map<uint32_t, int64_t> konst;  // GPR -> known constant
  auto regOff = [&](uint32_t reg) -> std::optional<int64_t> {
    if (reg == 1)
      return spOff;
    auto it = spRel.find(reg);
    return it == spRel.end() ? std::nullopt : std::optional<int64_t>(it->second);
  };
  auto forget = [&](uint32_t reg) {
    spRel.erase(reg);
    konst.erase(reg);
  };
  auto clobberVolatiles = [&]() {
    for (auto* map : {&spRel, &konst})
      for (auto it = map->begin(); it != map->end();)
        it = it->first < 14 ? map->erase(it) : std::next(it);
  };
  auto checkRestore = [&](int n, uint32_t address) -> std::optional<HostSavePlan> {
    if (!saved)
      return HostSavePlan{R::kSaveNotAtEntry, 0, address};
    if (n < first)
      return HostSavePlan{R::kRestoreWider, 0, address};
    if (spOff != 0)
      return HostSavePlan{R::kRestoreFrame, 0, address};
    return std::nullopt;
  };
  // A pointer into the area refuses the function. One at its lower edge is
  // the frame pointer the FPR and VMX helpers take in r12 (they store below).
  auto pointerOk = [&](uint32_t rd, int64_t value) {
    if (value > dangerLo && value < dangerHi)
      return false;
    return value != dangerLo || rd == 12;
  };

  ppc_insn insn;
  for (size_t i = 0; i < words.size(); ++i) {
    const Word& w = words[i];
    if (labels.count(w.address)) {
      controlFlowSeen = true;
      spRel.clear();
      konst.clear();
      if (bodyOff)
        spOff = *bodyOff;
    }
    Disassemble(w.data, 4, w.address, insn);
    if (!insn.opcode) {
      if (w.raw != 0)
        return {R::kUndecoded, 0, w.address};
      continue;
    }
    const std::string_view name = insn.opcode->name;
    const auto& ops = insn.operands;
    const uint32_t op = PPC_OP(w.raw);

    // --- Branches ---
    if (op == PPC_OP_B || op == PPC_OP_BC) {
      const uint32_t target = branchTarget(w);
      const bool linked = PPC_BL(w.raw);
      if (fn.containsAddress(target) && !(linked && target == fn.base())) {
        if (!saved)
          return {R::kSaveNotAtEntry, 0, w.address};
        controlFlowSeen = true;
        continue;
      }
      const std::string_view callee = calleeName(target);
      if (linked) {
        if (i == saveIndex) {
          if (controlFlowSeen || spOff != 0)
            return {R::kSaveNotAtEntry, 0, w.address};
          saved = true;
        } else if (const int n = SaveRestoreHelperFirstRegister(callee, "__restgprlr_"); n >= 0) {
          if (auto refusal = checkRestore(n, w.address))
            return *refusal;
        } else if ((cfg.longJmpAddress && target == cfg.longJmpAddress) ||
                   (cfg.setJmpAddress && target == cfg.setJmpAddress)) {
          return {R::kSetJmp, 0, w.address};
        }
        clobberVolatiles();
        continue;
      }
      if (const int n = SaveRestoreHelperFirstRegister(callee, "__restgprlr_"); n >= 0) {
        if (auto refusal = checkRestore(n, w.address))
          return *refusal;
        continue;
      }
      return {R::kTailBranch, 0, w.address};
    }
    if (op == PPC_OP_CTR) {
      const uint32_t xop = PPC_XOP(w.raw);
      if (xop == 528) {  // bcctr
        if (PPC_BL(w.raw)) {
          clobberVolatiles();
          continue;
        }
        if (!switchBctrs.count(w.address))
          return {R::kIndirectBranch, 0, w.address};
        if (!saved)
          return {R::kSaveNotAtEntry, 0, w.address};
        controlFlowSeen = true;
        continue;
      }
      if (xop == 16) {  // bclr: a return, or blrl, a call through LR
        if (PPC_BL(w.raw))
          clobberVolatiles();
        continue;
      }
    }

    // --- Loads, stores and cache operations ---
    const bool isStore = name.starts_with("st");
    const bool isCache = name.starts_with("dcb") || name.starts_with("icb");
    const bool isLoad = name.starts_with("l") && !name.starts_with("li") &&
                        !name.starts_with("lvsl") && !name.starts_with("lvsr");
    if (isLoad || isStore || isCache) {
      bool indexed = isCache || name.find('x') != std::string_view::npos;
      uint32_t ra = 0, rb = 0;
      int64_t disp = 0;
      if (isCache) {
        ra = ops[0];
        rb = ops[1];
      } else if (indexed) {
        ra = ops[1];
        rb = ops[2];
      } else {
        ra = ops[2];
        disp = static_cast<int32_t>(ops[1]);
      }
      const bool update = !isCache && (name.ends_with("u") || name.ends_with("ux"));
      const bool gprData = !isCache && !name.starts_with("lf") && !name.starts_with("stf") &&
                           !name.starts_with("lv") && !name.starts_with("stv");

      // The prologue's frame allocation and its back chain.
      if (name == "stwu" && ops[0] == 1 && ra == 1) {
        if (bodyOff)
          return {R::kDynamicFrame, 0, w.address};
        bodyOff = spOff + disp;
        spOff = *bodyOff;
        continue;
      }
      if (update && ra == 1)
        return {R::kDynamicFrame, 0, w.address};
      if (isStore && gprData && ops[0] == 1)
        return {R::kSpStored, 0, w.address};

      std::optional<int64_t> e;
      if (indexed) {
        const std::optional<int64_t> a = ra == 0 ? std::nullopt : regOff(ra);
        const std::optional<int64_t> b = regOff(rb);
        if (a && b)
          return {R::kSpArithmetic, 0, w.address};
        if (a || b) {
          const uint32_t other = a ? rb : ra;
          int64_t k = 0;
          if (other != 0 || a) {
            auto it = konst.find(other);
            if (it == konst.end())
              return {R::kSpIndexedUnknown, 0, w.address};
            k = it->second;
          }
          e = (a ? *a : *b) + k;
        }
      } else if (ra != 0) {
        e = regOff(ra);
        if (e)
          *e += disp;
      }
      if (e) {
        int64_t lo = *e;
        int64_t hi = lo + AccessSize(name);
        if (name.starts_with("lmw") || name.starts_with("stmw"))
          hi = lo + 4 * (32 - static_cast<int64_t>(ops[0]));
        if (name.starts_with("lv") || name.starts_with("stv")) {
          lo &= ~int64_t{15};
          hi = lo + 16;
        }
        if (isCache) {
          lo &= ~int64_t{127};
          hi = lo + 128;
        }
        if (overlapsArea(lo, hi))
          return {R::kSaveSlotAccess, 0, w.address};
      }

      if (isLoad && gprData) {
        const uint32_t rd = ops[0];
        if (name.starts_with("lmw")) {
          if (rd <= 1)
            return {R::kR1Write, 0, w.address};
          for (uint32_t r = rd; r < 32; ++r)
            forget(r);
        } else if (rd == 1) {
          // The back chain reload: r1 takes the entry stack pointer back.
          if (name == "lwz" && ra == 1 && disp == 0 && bodyOff && spOff == *bodyOff)
            spOff = 0;
          else
            return {R::kR1Write, 0, w.address};
        } else {
          forget(rd);
        }
      }
      if (update)
        forget(ra);
      continue;
    }

    // --- Integer operations on r1 and the registers derived from it ---
    if (name == "addi" || name == "mr") {
      const uint32_t rd = ops[0];
      const uint32_t ra = ops[1];
      const int64_t imm = name == "addi" ? static_cast<int32_t>(ops[2]) : 0;
      if (rd == 1) {
        if (name == "addi" && ra == 1 && bodyOff && spOff == *bodyOff && spOff + imm == 0) {
          spOff = 0;
          continue;
        }
        return {R::kR1Write, 0, w.address};
      }
      if (auto a = regOff(ra)) {
        const int64_t value = *a + imm;
        if (!pointerOk(rd, value))
          return {R::kSaveAreaAddress, 0, w.address};
        konst.erase(rd);
        spRel[rd] = value;
        continue;
      }
      auto k = konst.find(ra);
      const std::optional<int64_t> known =
          k == konst.end() ? std::nullopt : std::optional<int64_t>(k->second + imm);
      forget(rd);
      if (known)
        konst[rd] = *known;
      continue;
    }
    if (name == "li" || name == "lis") {
      const uint32_t rd = ops[0];
      if (rd == 1)
        return {R::kR1Write, 0, w.address};
      forget(rd);
      const int64_t imm = static_cast<int32_t>(ops[1]);
      konst[rd] = name == "li" ? imm : static_cast<int64_t>(static_cast<int32_t>(
                                           static_cast<uint32_t>(imm) << 16));
      continue;
    }
    if (FirstOperandNotGprDest(name))
      continue;
    const uint32_t rd = ops[0];
    if (rd == 1)
      return {R::kR1Write, 0, w.address};
    if (!name.starts_with("mf") && (ops[1] == 1 || spRel.count(ops[1])))
      return {R::kSpArithmetic, 0, w.address};
    if (ThirdOperandIsGpr(name) && (ops[2] == 1 || spRel.count(ops[2])))
      return {R::kSpArithmetic, 0, w.address};
    forget(rd);
  }

  // A split function that falls through into the next entry continues in
  // another function on this frame (emitCpp's fall-through tail call).
  if (!fn.blocks().empty() && fn.blocks().back().end() == fn.end()) {
    const auto* next = ctx.graph.getFunction(fn.end());
    const auto* last = ctx.binary.translate(fn.end() - 4);
    if (next && next != &fn && last) {
      const uint32_t raw = load_and_swap<uint32_t>(last);
      const uint32_t op = PPC_OP(raw);
      if (!((op == PPC_OP_B || op == PPC_OP_CTR) && !PPC_BL(raw)))
        return {R::kFallthrough, 0, fn.end() - 4};
    }
  }
  if (!saved)
    return {R::kSaveNotAtEntry, 0, saveAddress};
  return {R::kTransformed, first, saveAddress};
}

//=============================================================================
// host_local_lr: the LR-slot scan (NEAR_NATIVE_RESEARCH.md NN-1.3)
//=============================================================================
// The LR save slot is the word at entry_sp - 8. A function may keep it in the
// `rex_saved_lr` local (BuilderContext::emitHostLocalSaveRestore, build_stw,
// build_lwz) only if the slot's single write is the prologue's (the
// __savegprlr_N call that host_local_register_saves already inlines, or one
// explicit `stw rS,-8(r1)`) in the straight-line code from the entry, and
// every other access is a whole-word reload after it (an `lwz` the emitter
// rewrites, or a __restgprlr_N epilogue). Nothing else may reach the slot: no
// pointer to it, no r1 the tracking loses, no dynamic frame, no unwinder
// (exception info), no hook, no call at the entry stack pointer (the callee's
// slot would be the same word), and no tail branch, fall-through or indirect
// jump into code that may read the slot before writing it (a split function
// or an out-of-line epilogue). The tracking follows ScanHostLocalSaves.

enum class HostLrReason : uint8_t {
  kTransformed,
  kNoSlot,
  kKeepList,
  kLocalized,
  kInteriorResume,
  kSharesRegisters,
  kExceptionInfo,
  kMidAsmHook,
  kHelpersKept,
  kHelperRestore,
  kNotAtEntry,
  kMultipleStores,
  kStoreNotAtEntry,
  kLoadBeforeStore,
  kSlotAccess,
  kSlotAddress,
  kCallAtEntrySp,
  kSetJmp,
  kTailBranch,
  kIndirectBranch,
  kFallthrough,
  kDynamicFrame,
  kR1Write,
  kSpStored,
  kSpArithmetic,
  kSpIndexedUnknown,
  kLabelFrame,
  kUndecoded,
  kCount,
};

constexpr const char* kHostLrReasonNames[] = {
    "transformed",        "no_slot",           "keep_list",        "non_volatile_as_local",
    "interior_resume",    "shares_registers",  "exception_info",   "mid_asm_hook",
    "helpers_kept",       "helper_restore",    "not_at_entry",     "multiple_stores",
    "store_not_at_entry", "load_before_store", "slot_access",      "slot_address",
    "call_at_entry_sp",   "setjmp_longjmp",    "tail_branch",      "indirect_branch",
    "fallthrough",        "dynamic_frame",     "r1_write",         "sp_stored",
    "sp_arithmetic",      "sp_indexed_unknown", "label_frame",      "undecoded",
};
static_assert(std::size(kHostLrReasonNames) == static_cast<size_t>(HostLrReason::kCount));

std::array<std::atomic<uint64_t>, static_cast<size_t>(HostLrReason::kCount)> gHostLrStats{};
std::atomic<uint64_t> gHostLrHelperForm{0};
std::atomic<uint64_t> gHostLrExplicitForm{0};
std::atomic<uint64_t> gHostLrLoadsRewritten{0};

/// What one walk of a function found about its LR slot.
struct LrSlotScan {
  HostLrReason refusal = HostLrReason::kTransformed;  ///< the walk lost track of r1
  uint32_t refusalAddress = 0;
  bool readsLr = false;        ///< an mflr anywhere
  bool savesLr = false;        ///< a __savegprlr_N call or a `stw rS,-8(r1)` anywhere
  bool helperSave = false;     ///< a `bl __savegprlr_N` at the entry stack pointer
  uint32_t store = 0;          ///< the explicit whole-word slot store
  bool writeFirst = false;     ///< the first slot access is a write, in the entry code
  bool otherAccess = false;    ///< a slot access that is neither the write nor a reload
  uint32_t otherAddress = 0;
  bool readBeforeWrite = false;
  uint32_t readBeforeWriteAddress = 0;
  bool multipleWrites = false;
  uint32_t multipleWritesAddress = 0;
  bool helperRestore = false;  ///< a __restgprlr_N call or tail (reads the slot)
  bool callAtEntrySp = false;  ///< a non-helper call with r1 at the entry value
  uint32_t callAtEntrySpAddress = 0;
  bool indirect = false;       ///< a bcctr that is not a switch
  uint32_t indirectAddress = 0;
  bool fallthrough = false;
  std::vector<uint32_t> loads;                       ///< `lwz` reloads of the slot
  std::vector<std::pair<uint32_t, uint32_t>> tails;  ///< (site, target) of tail branches

  bool touchesSlot() const {
    return helperSave || helperRestore || store || !loads.empty() || otherAccess ||
           readBeforeWrite || multipleWrites;
  }
};

LrSlotScan WalkLrSlot(const FunctionNode& fn, const EmitContext& ctx) {
  using R = HostLrReason;
  const auto& cfg = ctx.config;
  LrSlotScan scan;
  auto refuse = [&](R reason, uint32_t address) {
    scan.refusal = reason;
    scan.refusalAddress = address;
    return scan;
  };
  struct Word {
    uint32_t address;
    uint32_t raw;
    const uint8_t* data;
  };
  std::vector<Word> words;
  for (const auto& block : fn.blocks()) {
    const uint8_t* data = ctx.binary.translate(block.base);
    if (!data)
      continue;
    for (uint32_t address = block.base; address < block.end(); address += 4) {
      const uint8_t* at = data + (address - block.base);
      words.push_back({address, load_and_swap<uint32_t>(at), at});
    }
  }
  auto calleeName = [&](uint32_t target) -> std::string_view {
    const FunctionNode* callee = ctx.graph.getFunction(target);
    return callee ? std::string_view(callee->name()) : std::string_view{};
  };
  auto branchTarget = [](const Word& w) {
    return PPC_OP(w.raw) == PPC_OP_B ? w.address + PPC_BI(w.raw) : w.address + PPC_BD(w.raw);
  };
  // Whether the function saves LR at all, before any tracking: an mflr and
  // the helper call or the prologue's `stw rS,-8(r1)`. A leaf that only uses
  // the red zone word as scratch has no LR slot to move.
  for (const Word& w : words) {
    if ((w.raw & 0xFC1FFFFFu) == 0x7C0802A6u)  // mflr rD
      scan.readsLr = true;
    if ((w.raw & 0xFC1FFFFFu) == 0x9001FFF8u ||  // stw rS,-8(r1)
        (PPC_OP(w.raw) == PPC_OP_B && PPC_BL(w.raw) &&
         SaveRestoreHelperFirstRegister(calleeName(branchTarget(w)), "__savegprlr_") >= 0))
      scan.savesLr = true;
  }
  if (words.empty() || words.front().address != fn.base())
    return refuse(R::kNotAtEntry, fn.base());
  auto isHelper = [](std::string_view name) {
    return name.starts_with("__save") || name.starts_with("__rest");
  };

  // Labels: internal branch targets and switch cases.
  std::unordered_set<uint32_t> labels;
  std::unordered_set<uint32_t> switchBctrs;
  std::unordered_map<uint32_t, std::vector<uint32_t>> switchTargets;  // bctr -> cases
  for (const auto& jt : fn.jumpTables()) {
    switchBctrs.insert(jt.bctrAddress);
    labels.insert(jt.targets.begin(), jt.targets.end());
    auto& cases = switchTargets[jt.bctrAddress];
    cases.insert(cases.end(), jt.targets.begin(), jt.targets.end());
  }
  for (const Word& w : words) {
    const uint32_t op = PPC_OP(w.raw);
    if ((op == PPC_OP_B || op == PPC_OP_BC) && fn.containsAddress(branchTarget(w)) &&
        !(PPC_BL(w.raw) && branchTarget(w) == fn.base()))
      labels.insert(branchTarget(w));
    auto st = cfg.switchTables.find(w.address);
    if (st != cfg.switchTables.end()) {
      switchBctrs.insert(w.address);
      labels.insert(st->second.targets.begin(), st->second.targets.end());
      auto& cases = switchTargets[w.address];
      cases.insert(cases.end(), st->second.targets.begin(), st->second.targets.end());
    }
    if (op == PPC_OP_CTR && PPC_XOP(w.raw) == 528 && !PPC_BL(w.raw) &&
        !switchBctrs.count(w.address)) {
      FunctionScanner scanner(ctx.binary);
      if (auto jt = scanner.detect_jump_table(w.address)) {
        switchBctrs.insert(w.address);
        labels.insert(jt->targets.begin(), jt->targets.end());
        auto& cases = switchTargets[w.address];
        cases.insert(cases.end(), jt->targets.begin(), jt->targets.end());
      }
    }
  }

  // The slot, relative to the entry stack pointer.
  constexpr int64_t kSlotLo = -8;
  constexpr int64_t kSlotHi = -4;
  bool written = false;
  bool accessed = false;
  auto noteWrite = [&](uint32_t address, bool inEntryCode) {
    if (written) {
      if (!scan.multipleWrites) {
        scan.multipleWrites = true;
        scan.multipleWritesAddress = address;
      }
      return false;
    }
    written = true;
    // First means before any other slot access and before anything that could
    // run other code on this stack pointer.
    if (!accessed && inEntryCode && !scan.callAtEntrySp && scan.tails.empty() && !scan.indirect)
      scan.writeFirst = true;
    accessed = true;
    return true;
  };
  auto noteRead = [&](uint32_t address) {
    if (!written && !scan.readBeforeWrite) {
      scan.readBeforeWrite = true;
      scan.readBeforeWriteAddress = address;
    }
    accessed = true;
  };
  auto noteOther = [&](uint32_t address) {
    if (!scan.otherAccess) {
      scan.otherAccess = true;
      scan.otherAddress = address;
    }
    accessed = true;
  };
  auto noteCallAtEntrySp = [&](uint32_t address) {
    if (!scan.callAtEntrySp) {
      scan.callAtEntrySp = true;
      scan.callAtEntrySpAddress = address;
    }
  };

  int64_t spOff = 0;  // r1 minus the entry stack pointer
  std::optional<int64_t> bodyOff;
  bool controlFlowSeen = false;  // a label or internal branch since entry
  std::unordered_map<uint32_t, int64_t> spRel;  // GPR -> entry-relative stack address
  std::unordered_map<uint32_t, int64_t> konst;  // GPR -> known constant
  auto regOff = [&](uint32_t reg) -> std::optional<int64_t> {
    if (reg == 1)
      return spOff;
    auto it = spRel.find(reg);
    return it == spRel.end() ? std::nullopt : std::optional<int64_t>(it->second);
  };
  auto forget = [&](uint32_t reg) {
    spRel.erase(reg);
    konst.erase(reg);
  };
  auto clobberVolatiles = [&]() {
    for (auto* map : {&spRel, &konst})
      for (auto it = map->begin(); it != map->end();)
        it = it->first < 14 ? map->erase(it) : std::next(it);
  };
  // A pointer into the slot escapes the tracking. r12 = the slot address is
  // the frame pointer the FPR and VMX helpers take when no GPR is saved (they
  // store below it); it may reach only those helpers.
  auto pointerOk = [&](uint32_t rd, int64_t value) {
    if (value > kSlotLo && value < kSlotHi)
      return false;
    return value != kSlotLo || rd == 12;
  };
  auto slotPointerLive = [&]() {
    auto it = spRel.find(12);
    return it != spRel.end() && it->second == kSlotLo;
  };

  // The walk assumes r1 at a label is the body's stack pointer (the entry's
  // when no frame is allocated yet); every way into the label must agree, or
  // a reload of the slot after an epilogue could be placed at the wrong word.
  std::unordered_map<uint32_t, int64_t> labelSp;                // label -> assumed r1
  std::vector<std::pair<uint32_t, int64_t>> edges;              // (target, r1 at the branch)
  bool mayFallThrough = false;
  auto noteEdge = [&](uint32_t target) { edges.emplace_back(target, spOff); };
  auto alwaysBranches = [](uint32_t raw) {
    const uint32_t bo = (raw >> 21) & 31;
    return (bo & 0x14) == 0x14;
  };

  ppc_insn insn;
  for (size_t i = 0; i < words.size(); ++i) {
    const Word& w = words[i];
    if (labels.count(w.address)) {
      controlFlowSeen = true;
      spRel.clear();
      konst.clear();
      const int64_t assumed = bodyOff ? *bodyOff : spOff;
      if (mayFallThrough && i > 0 && spOff != assumed)
        return refuse(R::kLabelFrame, w.address);
      spOff = assumed;
      labelSp[w.address] = assumed;
    }
    mayFallThrough = true;
    Disassemble(w.data, 4, w.address, insn);
    if (!insn.opcode) {
      if (w.raw != 0)
        return refuse(R::kUndecoded, w.address);
      continue;
    }
    const std::string_view name = insn.opcode->name;
    const auto& ops = insn.operands;
    const uint32_t op = PPC_OP(w.raw);

    // --- Branches ---
    if (op == PPC_OP_B || op == PPC_OP_BC) {
      const uint32_t target = branchTarget(w);
      const bool linked = PPC_BL(w.raw);
      if (fn.containsAddress(target) && !(linked && target == fn.base())) {
        controlFlowSeen = true;
        noteEdge(target);
        if (op == PPC_OP_B || alwaysBranches(w.raw))
          mayFallThrough = linked;
        continue;
      }
      const std::string_view callee = calleeName(target);
      if (!linked && (op == PPC_OP_B || alwaysBranches(w.raw)))
        mayFallThrough = false;
      if (linked) {
        if (SaveRestoreHelperFirstRegister(callee, "__savegprlr_") >= 0) {
          // The helper stores r12 at r1 - 8: the slot only at the entry r1.
          if (spOff == 0) {
            scan.helperSave = true;
            noteWrite(w.address, !controlFlowSeen);
          } else {
            noteOther(w.address);
          }
        } else if (SaveRestoreHelperFirstRegister(callee, "__restgprlr_") >= 0) {
          scan.helperRestore = true;
          if (spOff == 0)
            noteRead(w.address);
          else
            noteOther(w.address);
        } else if ((cfg.longJmpAddress && target == cfg.longJmpAddress) ||
                   (cfg.setJmpAddress && target == cfg.setJmpAddress)) {
          return refuse(R::kSetJmp, w.address);
        } else if (!isHelper(callee)) {
          if (slotPointerLive())
            return refuse(R::kSlotAddress, w.address);
          if (spOff == 0)
            noteCallAtEntrySp(w.address);
        }
        clobberVolatiles();
        continue;
      }
      if (SaveRestoreHelperFirstRegister(callee, "__restgprlr_") >= 0) {
        scan.helperRestore = true;
        if (spOff == 0)
          noteRead(w.address);
        else
          noteOther(w.address);
        continue;
      }
      if (!isHelper(callee) && slotPointerLive())
        return refuse(R::kSlotAddress, w.address);
      scan.tails.emplace_back(w.address, target);
      continue;
    }
    if (op == PPC_OP_CTR) {
      const uint32_t xop = PPC_XOP(w.raw);
      if (xop == 528 || xop == 16) {  // bcctr, bclr
        if (PPC_BL(w.raw)) {          // bctrl, blrl: a call to an unknown target
          if (slotPointerLive())
            return refuse(R::kSlotAddress, w.address);
          if (spOff == 0)
            noteCallAtEntrySp(w.address);
          clobberVolatiles();
          continue;
        }
        if (alwaysBranches(w.raw))
          mayFallThrough = false;
        if (xop == 16)
          continue;  // a return
        if (!switchBctrs.count(w.address)) {
          if (slotPointerLive())
            return refuse(R::kSlotAddress, w.address);
          if (!scan.indirect) {
            scan.indirect = true;
            scan.indirectAddress = w.address;
          }
          continue;
        }
        controlFlowSeen = true;
        if (auto cases = switchTargets.find(w.address); cases != switchTargets.end())
          for (uint32_t target : cases->second)
            noteEdge(target);
        continue;
      }
    }

    // --- Loads, stores and cache operations ---
    const bool isStore = name.starts_with("st");
    const bool isCache = name.starts_with("dcb") || name.starts_with("icb");
    const bool isLoad = name.starts_with("l") && !name.starts_with("li") &&
                        !name.starts_with("lvsl") && !name.starts_with("lvsr");
    if (isLoad || isStore || isCache) {
      bool indexed = isCache || name.find('x') != std::string_view::npos;
      uint32_t ra = 0, rb = 0;
      int64_t disp = 0;
      if (isCache) {
        ra = ops[0];
        rb = ops[1];
      } else if (indexed) {
        ra = ops[1];
        rb = ops[2];
      } else {
        ra = ops[2];
        disp = static_cast<int32_t>(ops[1]);
      }
      const bool update = !isCache && (name.ends_with("u") || name.ends_with("ux"));
      const bool gprData = !isCache && !name.starts_with("lf") && !name.starts_with("stf") &&
                           !name.starts_with("lv") && !name.starts_with("stv");

      // The prologue's frame allocation and its back chain.
      if (name == "stwu" && ops[0] == 1 && ra == 1) {
        if (bodyOff)
          return refuse(R::kDynamicFrame, w.address);
        if (spOff + disp < kSlotHi && spOff + disp + 4 > kSlotLo)
          noteOther(w.address);
        bodyOff = spOff + disp;
        spOff = *bodyOff;
        continue;
      }
      if (update && ra == 1)
        return refuse(R::kDynamicFrame, w.address);
      if (isStore && gprData && ops[0] == 1)
        return refuse(R::kSpStored, w.address);

      std::optional<int64_t> e;
      if (indexed) {
        const std::optional<int64_t> a = ra == 0 ? std::nullopt : regOff(ra);
        const std::optional<int64_t> b = regOff(rb);
        if (a && b)
          return refuse(R::kSpArithmetic, w.address);
        if (a || b) {
          const uint32_t other = a ? rb : ra;
          int64_t k = 0;
          if (other != 0 || a) {
            auto it = konst.find(other);
            if (it == konst.end())
              return refuse(R::kSpIndexedUnknown, w.address);
            k = it->second;
          }
          e = (a ? *a : *b) + k;
        }
      } else if (ra != 0) {
        e = regOff(ra);
        if (e)
          *e += disp;
      }
      if (e) {
        int64_t lo = *e;
        int64_t hi = lo + AccessSize(name);
        if (name.starts_with("lmw") || name.starts_with("stmw"))
          hi = lo + 4 * (32 - static_cast<int64_t>(ops[0]));
        if (name.starts_with("lv") || name.starts_with("stv")) {
          lo &= ~int64_t{15};
          hi = lo + 16;
        }
        if (isCache) {
          lo &= ~int64_t{127};
          hi = lo + 128;
        }
        if (lo < kSlotHi && hi > kSlotLo) {
          if (name == "stw" && lo == kSlotLo) {
            if (noteWrite(w.address, !controlFlowSeen))
              scan.store = w.address;
          } else if (name == "lwz" && lo == kSlotLo && ops[0] != 1) {
            noteRead(w.address);
            scan.loads.push_back(w.address);
          } else {
            noteOther(w.address);
          }
        }
      }

      if (isLoad && gprData) {
        const uint32_t rd = ops[0];
        if (name.starts_with("lmw")) {
          if (rd <= 1)
            return refuse(R::kR1Write, w.address);
          for (uint32_t r = rd; r < 32; ++r)
            forget(r);
        } else if (rd == 1) {
          // The back chain reload: r1 takes the entry stack pointer back.
          if (name == "lwz" && ra == 1 && disp == 0 && bodyOff && spOff == *bodyOff)
            spOff = 0;
          else
            return refuse(R::kR1Write, w.address);
        } else if (name == "lwz" && e && bodyOff && *e == *bodyOff && spOff == *bodyOff) {
          // A copy of the back chain is the entry stack pointer.
          konst.erase(rd);
          spRel[rd] = 0;
        } else {
          forget(rd);
        }
      }
      if (update)
        forget(ra);
      continue;
    }

    // --- Integer operations on r1 and the registers derived from it ---
    if (name == "addi" || name == "mr") {
      const uint32_t rd = ops[0];
      const uint32_t ra = ops[1];
      const int64_t imm = name == "addi" ? static_cast<int32_t>(ops[2]) : 0;
      if (rd == 1) {
        if (name == "addi" && ra == 1 && bodyOff && spOff == *bodyOff && spOff + imm == 0) {
          spOff = 0;
          continue;
        }
        return refuse(R::kR1Write, w.address);
      }
      if (auto a = regOff(ra)) {
        const int64_t value = *a + imm;
        if (!pointerOk(rd, value))
          return refuse(R::kSlotAddress, w.address);
        konst.erase(rd);
        spRel[rd] = value;
        continue;
      }
      auto k = konst.find(ra);
      const std::optional<int64_t> known =
          k == konst.end() ? std::nullopt : std::optional<int64_t>(k->second + imm);
      forget(rd);
      if (known)
        konst[rd] = *known;
      continue;
    }
    if (name == "li" || name == "lis") {
      const uint32_t rd = ops[0];
      if (rd == 1)
        return refuse(R::kR1Write, w.address);
      forget(rd);
      const int64_t imm = static_cast<int32_t>(ops[1]);
      konst[rd] = name == "li" ? imm : static_cast<int64_t>(static_cast<int32_t>(
                                           static_cast<uint32_t>(imm) << 16));
      continue;
    }
    if (FirstOperandNotGprDest(name))
      continue;
    const uint32_t rd = ops[0];
    if (rd == 1)
      return refuse(R::kR1Write, w.address);
    if (!name.starts_with("mf") && (ops[1] == 1 || spRel.count(ops[1])))
      return refuse(R::kSpArithmetic, w.address);
    if (ThirdOperandIsGpr(name) && (ops[2] == 1 || spRel.count(ops[2])))
      return refuse(R::kSpArithmetic, w.address);
    forget(rd);
  }

  for (const auto& [target, sp] : edges) {
    auto it = labelSp.find(target);
    if (it != labelSp.end() && it->second != sp)
      return refuse(R::kLabelFrame, target);
  }

  // A split function that falls through into the next entry continues in
  // another function on this frame (emitCpp's fall-through tail call).
  if (!fn.blocks().empty() && fn.blocks().back().end() == fn.end()) {
    const auto* next = ctx.graph.getFunction(fn.end());
    const auto* last = ctx.binary.translate(fn.end() - 4);
    if (next && next != &fn && last) {
      const uint32_t raw = load_and_swap<uint32_t>(last);
      const uint32_t op = PPC_OP(raw);
      if (!((op == PPC_OP_B || op == PPC_OP_CTR) && !PPC_BL(raw)))
        scan.fallthrough = true;
    }
  }
  return scan;
}

/// Whether code reached by a tail branch to `target` may read the slot the
/// branching function wrote before writing it itself: a split function, an
/// out-of-line epilogue, or anything the walk cannot follow. A function that
/// writes its own slot first is safe, and so is one that never touches it,
/// calls nothing at the entry stack pointer and branches nowhere else.
/// Imports are host code, which never reads the guest LR slot.
bool MayReadCallerLrSlot(uint32_t target, const EmitContext& ctx) {
  const FunctionNode* callee = ctx.graph.getFunction(target);
  if (!callee || callee->base() != target)
    return true;
  if (callee->isImport())
    return false;
  if (callee->blocks().empty() || callee->hasExceptionInfo() || callee->sharesRegisters())
    return true;
  const LrSlotScan scan = WalkLrSlot(*callee, ctx);
  if (scan.writeFirst)
    return false;
  if (scan.refusal != HostLrReason::kTransformed)
    return true;
  return scan.touchesSlot() || scan.callAtEntrySp || scan.indirect || scan.fallthrough ||
         !scan.tails.empty();
}

struct HostLrPlan {
  HostLrReason reason = HostLrReason::kNoSlot;
  uint32_t address = 0;
  uint32_t store = 0;
  std::vector<uint32_t> loads;
};

HostLrPlan ScanHostLocalLr(const FunctionNode& fn, const EmitContext& ctx,
                           const HostSavePlan& savePlan) {
  using R = HostLrReason;
  const auto& cfg = ctx.config;
  const LrSlotScan scan = WalkLrSlot(fn, ctx);
  if (!scan.readsLr || !(scan.savesLr || scan.store || scan.helperSave))
    return {R::kNoSlot};
  const uint32_t at = fn.base();
  if (std::binary_search(cfg.hostLocalLrKeep.begin(), cfg.hostLocalLrKeep.end(), at))
    return {R::kKeepList, at};
  if (cfg.nonVolatileRegistersAsLocalVariables)
    return {R::kLocalized, at};
  if (cfg.interiorResume)
    return {R::kInteriorResume, at};
  if (fn.sharesRegisters())
    return {R::kSharesRegisters, at};
  if (fn.hasExceptionInfo())
    return {R::kExceptionInfo, at};
  for (const auto& block : fn.blocks())
    for (uint32_t address = block.base; address < block.end(); address += 4)
      if (cfg.midAsmHooks.count(address))
        return {R::kMidAsmHook, address};
  if (scan.refusal != R::kTransformed)
    return {scan.refusal, scan.refusalAddress};
  // The save helper writes the slot itself unless host_local_register_saves
  // inlined it; the restore helper reads it unless the same plan covers it.
  const bool helperForm = savePlan.reason == HostSaveReason::kTransformed;
  if (!helperForm && (scan.helperSave || scan.helperRestore))
    return {scan.helperSave ? R::kHelpersKept : R::kHelperRestore, at};
  if (helperForm && !scan.helperSave)
    return {R::kHelpersKept, at};
  if (scan.multipleWrites)
    return {R::kMultipleStores, scan.multipleWritesAddress};
  if (scan.readBeforeWrite)
    return {R::kLoadBeforeStore, scan.readBeforeWriteAddress};
  if (scan.otherAccess)
    return {R::kSlotAccess, scan.otherAddress};
  if (!scan.writeFirst)
    return {R::kStoreNotAtEntry, scan.store ? scan.store : at};
  if (scan.callAtEntrySp)
    return {R::kCallAtEntrySp, scan.callAtEntrySpAddress};
  if (scan.indirect)
    return {R::kIndirectBranch, scan.indirectAddress};
  if (scan.fallthrough)
    return {R::kFallthrough, fn.end() - 4};
  for (const auto& [site, target] : scan.tails)
    if (MayReadCallerLrSlot(target, ctx))
      return {R::kTailBranch, site};
  HostLrPlan plan{R::kTransformed, at, scan.store, scan.loads};
  std::sort(plan.loads.begin(), plan.loads.end());
  return plan;
}

//=============================================================================
// NN-5.1 per-function specialisation (register_locals_functions,
// fixed_fp_mode_functions, single_precision_functions)
//=============================================================================

enum class SpecializeReason : uint8_t {
  kApplied,
  kSharesRegisters,
  kExceptionInfo,
  kInteriorResume,
  kMidAsmHook,
  kAbiPoisonChecks,
  kSetJmpOrSeh,
  kCount,
};

constexpr const char* kSpecializeReasonNames[] = {
    "applied",           "shares_registers", "exception_info", "interior_resume",
    "mid_asm_hook",      "abi_poison_checks", "setjmp_longjmp_or_seh",
};
static_assert(std::size(kSpecializeReasonNames) == static_cast<size_t>(SpecializeReason::kCount));

std::array<std::atomic<uint64_t>, static_cast<size_t>(SpecializeReason::kCount)> gRegisterLocalsStats{};
std::atomic<uint64_t> gFixedFpFunctions{0};
std::atomic<uint64_t> gFixedFpSites{0};
std::atomic<uint64_t> gIeeeFallbacks{0};
std::atomic<uint64_t> gSingleFunctions{0};
std::atomic<uint64_t> gSingleFmas{0};

/// Why a function listed in register_locals_functions keeps its registers in
/// ctx, from what is known before emission (kApplied when nothing).
SpecializeReason RegisterLocalsRefusal(const FunctionNode& fn, const EmitContext& ctx,
                                       bool hasResumeSwitch) {
  using R = SpecializeReason;
  // A funclet reads its owner's registers through ctx.
  if (fn.sharesRegisters())
    return R::kSharesRegisters;
  // An unwinder or SEH handler sees ctx, not the locals.
  if (fn.hasExceptionInfo())
    return R::kExceptionInfo;
  // A resume entry would start from ctx with the locals unset.
  if (hasResumeSwitch)
    return R::kInteriorResume;
  // Hooks take registers by reference or ctx itself.
  for (const auto& [address, hook] : ctx.config.midAsmHooks) {
    (void)hook;
    if (fn.containsAddress(address))
      return R::kMidAsmHook;
  }
  // The poison checks snapshot ctx before the publish at a call.
  if (ctx.config.abiPoisonChecks)
    return R::kAbiPoisonChecks;
  return R::kApplied;
}

/// A body form register_locals cannot carry, found after emission: the
/// setjmp context copy, longjmp and SEH (they copy or restore all of ctx).
bool HasWholeContextForms(const std::string& body) {
  return body.find("ppc_setjmp") != std::string::npos ||
         body.find("ppc_longjmp") != std::string::npos ||
         body.find("SEH_") != std::string::npos || body.find("ctx = env") != std::string::npos ||
         body.find("env = ctx") != std::string::npos;
}

/// register_locals_functions: every register local a body assigns, volatile
/// ones included. A line that starts with a register local assigns it
/// (`rN.u64 = `, `vN.u32[i] |= `, ...), and so does a vector store whose
/// destination is the local. Registers an inlined restore helper (`//
/// nv-helper`) assigns count too: the publish pass decides whether ctx still
/// holds the value they get back.
std::vector<std::string> WrittenAllRegisters(const std::string& body) {
  std::set<std::string> written;
  auto parseRegister = [](std::string_view text, size_t at, std::string& out, bool& nonVolatile) {
    if (at >= text.size())
      return false;
    char kind = text[at];
    if (kind != 'r' && kind != 'f' && kind != 'v')
      return false;
    size_t q = at + 1;
    while (q < text.size() && std::isdigit(static_cast<unsigned char>(text[q])))
      q++;
    if (q == at + 1 || q >= text.size() || (text[q] != '.' && text[q] != '[' && text[q] != ' '))
      return false;
    unsigned index = static_cast<unsigned>(std::stoul(std::string(text.substr(at + 1, q - at - 1))));
    if ((kind == 'v' && index >= 128) || (kind != 'v' && index >= 32))
      return false;
    nonVolatile = kind == 'v' ? ((index >= 14 && index < 32) || index >= 64) : index >= 14;
    out = std::string(text.substr(at, q - at));
    return true;
  };
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    if (end == std::string::npos)
      end = body.size();
    std::string_view line(body.data() + pos, end - pos);
    pos = end + 1;
    size_t first = line.find_first_not_of('\t');
    if (first == std::string_view::npos)
      continue;
    std::string_view code = line.substr(first);
    std::string reg;
    bool nonVolatile = false;
    if (code.starts_with("simde_mm_store")) {
      size_t open = code.find('(');
      size_t comma = code.find(',');
      if (open != std::string_view::npos && comma != std::string_view::npos) {
        size_t cast = code.rfind(')', comma);
        size_t start = (cast != std::string_view::npos && cast > open) ? cast + 1 : open + 1;
        if (parseRegister(code, start, reg, nonVolatile))
          written.insert(reg);
      }
      continue;
    }
    if (parseRegister(code, 0, reg, nonVolatile))
      written.insert(reg);
  }
  return std::vector<std::string>(written.begin(), written.end());
}

/// The register local a generated line assigns, if any: a line that starts
/// with a register local (`rN.u64 = `, `vN.u32[i] |= `, ...) or a vector
/// store whose destination operand is one. `nonVolatile` tells the class.
bool AssignedRegister(std::string_view code, std::string& reg, bool& nonVolatile) {
  auto parse = [&](size_t at) {
    if (at >= code.size())
      return false;
    char kind = code[at];
    if (kind != 'r' && kind != 'f' && kind != 'v')
      return false;
    size_t q = at + 1;
    while (q < code.size() && std::isdigit(static_cast<unsigned char>(code[q])))
      q++;
    if (q == at + 1 || q >= code.size() || (code[q] != '.' && code[q] != '[' && code[q] != ' '))
      return false;
    unsigned index = static_cast<unsigned>(std::stoul(std::string(code.substr(at + 1, q - at - 1))));
    if ((kind == 'v' && index >= 128) || (kind != 'v' && index >= 32))
      return false;
    nonVolatile = kind == 'v' ? ((index >= 14 && index < 32) || index >= 64) : index >= 14;
    reg = std::string(code.substr(at, q - at));
    return true;
  };
  if (code.starts_with("simde_mm_store")) {
    size_t open = code.find('(');
    size_t comma = code.find(',');
    if (open == std::string_view::npos || comma == std::string_view::npos)
      return false;
    size_t cast = code.rfind(')', comma);
    size_t start = (cast != std::string_view::npos && cast > open) ? cast + 1 : open + 1;
    return parse(start);
  }
  return parse(0);
}

/// register_locals_functions (NN-5.1): synchronize a body whose registers are
/// all locals with ctx. Invariant: a local not in the dirty set equals its ctx
/// field. Entry is clean (every local is loaded from ctx), an assignment makes
/// its register dirty, and a label's set is the union over every path into it
/// (fall-through, gotos, switch cases), iterated to a fixed point for loops.
/// Before a call, tail call or return the dirty registers and the
/// non-volatile CR fields go to ctx; after a call the volatile locals come
/// back (`reloadAfterCall`). A publish at the top level of the body leaves
/// everything clean; one inside a nested block may not run, so it clears
/// nothing. Lines the pass does not recognize change nothing, which can only
/// leave a register dirty for longer (an extra store), never clean too soon.
///
/// An inlined restore helper (`// nv-helper`) gives a non-volatile its entry
/// value back. That leaves it clean only while ctx still holds the entry
/// value, so a second set tracks the registers whose ctx field may have been
/// overwritten by a publish since entry ("moved"); a restore of a moved
/// register is a write like any other. (The offline equivalence check caught
/// the first version, which treated every restore as clean: a non-volatile
/// published before a call kept its computed value in ctx at the return.)
std::string SyncAllRegisterLocals(const std::string& body, const std::vector<std::string>& written,
                                  const std::string& crSpill, const std::string& reloadAfterCall,
                                  size_t& publishedStores) {
  using Set = std::bitset<256>;
  auto contains = [](std::string_view line, std::string_view what) {
    return line.find(what) != std::string_view::npos;
  };
  std::unordered_map<std::string, size_t> slot;
  for (size_t i = 0; i < written.size() && i < 256; ++i)
    slot.emplace(written[i], i);

  enum class Kind : uint8_t { kOther, kComment, kLabel, kPublish, kWrite, kRestore };
  struct Line {
    std::string_view text;
    Kind kind = Kind::kOther;
    bool topLevel = false;
    bool calls = false;
    bool leaves = false;
    bool ends = false;      // control does not fall through (top-level goto, return, tail call)
    size_t reg = SIZE_MAX;  // kWrite
    std::string_view label; // kLabel: its name; otherwise a goto or case target
  };
  std::vector<Line> lines;
  for (size_t pos = 0; pos < body.size();) {
    size_t end = body.find('\n', pos);
    if (end == std::string::npos)
      end = body.size();
    Line li;
    li.text = std::string_view(body.data() + pos, end - pos);
    pos = end + 1;
    std::string_view line = li.text;
    if (line.starts_with("loc_") && line.ends_with(":")) {
      li.kind = Kind::kLabel;
      li.label = line.substr(0, line.size() - 1);
      lines.push_back(li);
      continue;
    }
    size_t first = line.find_first_not_of('\t');
    std::string_view code = first == std::string_view::npos ? std::string_view{} : line.substr(first);
    if (code.empty() || code.starts_with("//")) {
      li.kind = Kind::kComment;
      lines.push_back(li);
      continue;
    }
    li.topLevel = first == 1;
    size_t at = code.find("loc_");
    if (at != std::string_view::npos) {
      size_t e = at + 4;
      while (e < code.size() && std::isxdigit(static_cast<unsigned char>(code[e])))
        e++;
      li.label = code.substr(at, e - at);
    }
    const bool leaves = contains(code, "return") || contains(code, "REX_TAIL_CALL");
    li.calls = contains(code, "ctx, base") || contains(code, "REX_CALL_INDIRECT_FUNC");
    li.leaves = leaves;
    if (leaves || li.calls) {
      li.kind = Kind::kPublish;
      li.ends = li.topLevel && leaves &&
                (code.starts_with("return") || code.starts_with("REX_TAIL_CALL") ||
                 code.starts_with("[[clang::musttail]]"));
    } else {
      std::string reg;
      bool nonVolatile = false;
      if (AssignedRegister(code, reg, nonVolatile)) {
        auto it = slot.find(reg);
        if (it != slot.end()) {
          li.kind = (nonVolatile && code.ends_with("// nv-helper")) ? Kind::kRestore : Kind::kWrite;
          li.reg = it->second;
        }
      }
      if (li.topLevel && code.starts_with("goto "))
        li.ends = true;
    }
    lines.push_back(li);
  }

  // Dataflow: the dirty and moved sets at every label.
  struct State {
    Set dirty, moved;
    bool operator!=(const State& o) const { return dirty != o.dirty || moved != o.moved; }
    State operator|(const State& o) const { return {dirty | o.dirty, moved | o.moved}; }
  };
  // A publish stores the dirty registers; a top-level one leaves them clean.
  auto publishState = [](State& st, bool topLevel) {
    st.moved |= st.dirty;
    if (topLevel)
      st.dirty.reset();
  };
  auto restore = [](State& st, size_t reg) {
    if (st.moved.test(reg))
      st.dirty.set(reg);
    else
      st.dirty.reset(reg);
  };
  // Control that does not fall through: nothing flows to the next line.
  auto endState = [](State& st) {
    st.dirty.reset();
    st.moved.reset();
  };
  std::unordered_map<std::string_view, State> labelIn;
  for (const Line& li : lines)
    if (li.kind == Kind::kLabel)
      labelIn.emplace(li.label, State{});
  auto edge = [&](std::string_view target, const State& state, bool& changed) {
    auto it = labelIn.find(target);
    if (it == labelIn.end())
      return;  // not a label of this body; nothing to propagate
    State next = it->second | state;
    if (next != it->second) {
      it->second = next;
      changed = true;
    }
  };
  for (bool changed = true; changed;) {
    changed = false;
    State state;
    for (const Line& li : lines) {
      switch (li.kind) {
        case Kind::kLabel: {
          State next = labelIn[li.label] | state;
          if (next != labelIn[li.label]) {
            labelIn[li.label] = next;
            changed = true;
          }
          state = next;
          break;
        }
        case Kind::kPublish:
          // A publish inside a nested branch also stores, so the edge sees
          // the state after it.
          publishState(state, li.topLevel);
          if (!li.label.empty())
            edge(li.label, state, changed);
          break;
        case Kind::kWrite:
          state.dirty.set(li.reg);
          break;
        case Kind::kRestore:
          restore(state, li.reg);
          break;
        default:
          if (!li.label.empty())
            edge(li.label, state, changed);
          break;
      }
      if (li.ends)
        endState(state);
    }
  }

  std::string out;
  out.reserve(body.size() + body.size() / 4);
  auto emit = [&out](std::string_view text) {
    out.append(text);
    out += '\n';
  };
  State state;
  for (const Line& li : lines) {
    switch (li.kind) {
      case Kind::kLabel:
        state = labelIn[li.label];
        break;
      case Kind::kPublish: {
        std::string publish = "\t" + crSpill;
        for (size_t i = 0; i < written.size() && i < 256; ++i) {
          if (!state.dirty.test(i))
            continue;
          publish += fmt::format("ctx.{0} = {0}; ", written[i]);
          ++publishedStores;
        }
        if (publish.size() > 1)
          emit(publish);
        emit(li.text);
        if (li.calls && !li.leaves && !reloadAfterCall.empty())
          emit(reloadAfterCall);
        publishState(state, li.topLevel);
        if (li.ends)
          endState(state);
        continue;
      }
      case Kind::kWrite:
        state.dirty.set(li.reg);
        break;
      case Kind::kRestore:
        restore(state, li.reg);
        break;
      default:
        break;
    }
    emit(li.text);
    if (li.ends)
      endState(state);
  }
  if (!body.empty() && body.back() != '\n' && !out.empty())
    out.pop_back();
  return out;
}

/// fixed_fp_mode_functions: guard every flush-mode toggle with the
/// `rex_fixed_fp` local the prologue reads once, or (`remove`, a
/// register_locals body that only runs in fixed mode) drop it. Returns the
/// number of toggles.
size_t HoistFixedFpMode(std::string& body, bool remove) {
  static constexpr std::string_view kToggles[] = {
      "ctx.fpscr.enableFlushMode();", "ctx.fpscr.disableFlushMode();",
      "ctx.fpscr.enableFlushModeUnconditional();", "ctx.fpscr.disableFlushModeUnconditional();"};
  std::string out;
  out.reserve(body.size() + body.size() / 16);
  size_t guarded = 0;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    const bool last = end == std::string::npos;
    if (last)
      end = body.size();
    std::string_view line(body.data() + pos, end - pos);
    pos = end + 1;
    size_t first = line.find_first_not_of('\t');
    std::string_view code = first == std::string_view::npos ? std::string_view{} : line.substr(first);
    bool toggle = false;
    for (std::string_view t : kToggles)
      toggle |= code == t;
    if (toggle && remove) {
      out.append(line.substr(0, first));
      out += "// flush-mode toggle dropped: fixed FP mode";
      ++guarded;
    } else if (toggle) {
      out.append(line.substr(0, first));
      out += "if (!rex_fixed_fp) ";
      out.append(code);
      ++guarded;
    } else {
      out.append(line);
    }
    if (!last)
      out += '\n';
  }
  body = std::move(out);
  return guarded;
}

}  // namespace

void LogSpecializedFunctionStats() {
  uint64_t listed = 0;
  for (const auto& c : gRegisterLocalsStats)
    listed += c.load();
  if (listed) {
    REXCODEGEN_INFO("register_locals_functions: {} of {} listed functions keep every register in "
                    "locals",
                    gRegisterLocalsStats[static_cast<size_t>(SpecializeReason::kApplied)].load(),
                    listed);
    for (size_t i = 1; i < gRegisterLocalsStats.size(); ++i) {
      const uint64_t count = gRegisterLocalsStats[i].load();
      if (count)
        REXCODEGEN_INFO("register_locals_functions:   kept in ctx {} ({})", count,
                        kSpecializeReasonNames[i]);
    }
  }
  if (gFixedFpFunctions.load())
    REXCODEGEN_INFO("fixed_fp_mode_functions: {} functions read the fixed FP mode once, {} "
                    "flush-mode toggles guarded or dropped",
                    gFixedFpFunctions.load(), gFixedFpSites.load());
  if (gIeeeFallbacks.load())
    REXCODEGEN_INFO("register_locals_functions: {} functions with flush-mode toggles run an "
                    "unspecialised ieee_ copy outside fixed FP mode",
                    gIeeeFallbacks.load());
  if (gSingleFunctions.load())
    REXCODEGEN_INFO("single_precision_functions: {} functions, {} fused multiply-adds computed in "
                    "single precision",
                    gSingleFunctions.load(), gSingleFmas.load());
}

void LogHostLocalSaveStats() {
  uint64_t total = 0;
  for (size_t i = 0; i < gHostSaveStats.size(); ++i)
    if (i != static_cast<size_t>(HostSaveReason::kNoSave))
      total += gHostSaveStats[i].load();
  REXCODEGEN_INFO("host_local_register_saves: {} of {} functions with a __savegprlr_N prologue "
                  "keep rN-r31 in host locals",
                  gHostSaveStats[static_cast<size_t>(HostSaveReason::kTransformed)].load(), total);
  for (size_t i = 0; i < gHostSaveStats.size(); ++i) {
    const uint64_t count = gHostSaveStats[i].load();
    if (count && i != static_cast<size_t>(HostSaveReason::kNoSave) &&
        i != static_cast<size_t>(HostSaveReason::kTransformed))
      REXCODEGEN_INFO("host_local_register_saves:   kept {} ({})", count, kHostSaveReasonNames[i]);
  }
}

void LogHostLocalLrStats() {
  uint64_t total = 0;
  for (size_t i = 0; i < gHostLrStats.size(); ++i)
    if (i != static_cast<size_t>(HostLrReason::kNoSlot))
      total += gHostLrStats[i].load();
  REXCODEGEN_INFO("host_local_lr: {} of {} functions that save LR keep the slot in a host "
                  "local ({} with the inlined __savegprlr_N, {} with an explicit stw; {} reloads "
                  "rewritten)",
                  gHostLrStats[static_cast<size_t>(HostLrReason::kTransformed)].load(), total,
                  gHostLrHelperForm.load(), gHostLrExplicitForm.load(),
                  gHostLrLoadsRewritten.load());
  for (size_t i = 0; i < gHostLrStats.size(); ++i) {
    const uint64_t count = gHostLrStats[i].load();
    if (count && i != static_cast<size_t>(HostLrReason::kNoSlot) &&
        i != static_cast<size_t>(HostLrReason::kTransformed))
      REXCODEGEN_INFO("host_local_lr:   kept {} ({})", count, kHostLrReasonNames[i]);
  }
}

std::vector<uint32_t> FunctionNode::resumableReturnAddresses(const BinaryView& binary) const {
  std::vector<uint32_t> addresses;

  for (const auto& block : blocks()) {
    const auto* blockData = binary.translate(block.base);
    if (!blockData) {
      continue;
    }

    for (uint32_t address = block.base; address < block.end(); address += 4) {
      const uint32_t instruction = load_and_swap<uint32_t>(blockData + address - block.base);
      const uint32_t opcode = PPC_OP(instruction);
      const uint32_t extendedOpcode = PPC_XOP(instruction);
      const bool isLinkedBranch =
          PPC_BL(instruction) &&
          (opcode == PPC_OP_B || opcode == PPC_OP_BC ||
           (opcode == PPC_OP_CTR && (extendedOpcode == 16 || extendedOpcode == 528)));
      if (!isLinkedBranch) {
        continue;
      }

      const uint32_t returnAddress = address + 4;
      if (containsAddress(returnAddress)) {
        addresses.push_back(returnAddress);
      }
    }
  }

  std::sort(addresses.begin(), addresses.end());
  addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
  return addresses;
}

std::string FunctionNode::emitCpp(const EmitContext& ctx) const {
  return emitCpp(ctx, true);
}

std::string FunctionNode::emitCpp(const EmitContext& ctx, bool allowSpecialization) const {
  if (authority() == FunctionAuthority::IMPORT) {
    return "";
  }

  std::string out;

  // --- Empty stub for functions with no blocks ---
  if (blocks().empty()) {
    REXCODEGEN_WARN("Function 0x{:08X} has no blocks - generating stub", base());

    std::string name;
    if (base() == ctx.entryPoint) {
      name = "xstart";
    } else if (!name_.empty()) {
      name = name_;
    } else {
      name = fmt::format("sub_{:08X}", base());
    }

    emit_println(out, "// STUB: Function at 0x{:08X} has no discovered code blocks", base());
    emit_println(out, "DEFINE_REX_FUNC({}) {{", name);
    emit_println(out, "\tREX_FUNC_PROLOGUE();");
    if (ctx.config.callProbes)
      emit_println(out, "\tREX_PROBE_CALL(0x{:08X});", base());
    emit_println(out, "}}\n");
    return out;
  }

  // --- Check for SEH exception info ---
  const SehExceptionInfo* sehInfo = nullptr;
  if (hasExceptionInfo()) {
    sehInfo = exceptionInfo()->asSeh();
    if (sehInfo && !sehInfo->scopes.empty()) {
      REXCODEGEN_TRACE("Function 0x{:08X} has {} SEH scopes", base(), sehInfo->scopes.size());
    }
  }

  // --- First pass: collect labels from all blocks ---
  std::unordered_set<size_t> labels;
  labels.reserve(64);
  // Without interior resume no return address is an entry, so no label and no
  // switch: the function has a single entry and its loops stay reducible.
  const auto resumeAddresses =
      ctx.config.interiorResume ? resumableReturnAddresses(ctx.binary) : std::vector<uint32_t>{};
  labels.insert(resumeAddresses.begin(), resumeAddresses.end());

  for (const auto& block : blocks()) {
    auto* blockData = reinterpret_cast<const uint32_t*>(ctx.binary.translate(block.base));
    if (!blockData)
      continue;

    for (size_t addr = block.base; addr < block.end(); addr += 4) {
      const uint32_t instruction =
          load_and_swap<uint32_t>((const uint8_t*)blockData + addr - block.base);
      if (!PPC_BL(instruction)) {
        const size_t op = PPC_OP(instruction);
        if (op == PPC_OP_B)
          labels.emplace(addr + PPC_BI(instruction));
        else if (op == PPC_OP_BC)
          labels.emplace(addr + PPC_BD(instruction));
      }

      // Labels from config switch tables
      auto stIt = ctx.config.switchTables.find(static_cast<uint32_t>(addr));
      if (stIt != ctx.config.switchTables.end()) {
        for (auto label : stIt->second.targets)
          labels.emplace(label);
      }

      // Labels and extern declarations from mid-asm hooks
      auto hookIt = ctx.config.midAsmHooks.find(static_cast<uint32_t>(addr));
      if (hookIt != ctx.config.midAsmHooks.end()) {
        if (hookIt->second.returnOnFalse || hookIt->second.returnOnTrue ||
            hookIt->second.jumpAddressOnFalse != 0 || hookIt->second.jumpAddressOnTrue != 0) {
          emit_print(out, "extern bool ");
        } else {
          emit_print(out, "extern void ");
        }

        emit_print(out, "{}(", hookIt->second.name);
        if (hookIt->second.context) {
          emit_print(out, "PPCContext& ctx, uint8_t* base");
        }
        for (auto& reg : hookIt->second.registers) {
          if (out.back() != '(')
            out += ", ";

          switch (reg[0]) {
            case 'c':
              if (reg == "ctr")
                emit_print(out, "PPCRegister& ctr");
              else
                emit_print(out, "PPCCRRegister& {}", reg);
              break;
            case 'x':
              emit_print(out, "PPCXERRegister& xer");
              break;
            case 'l':
              if (reg == "lr")
                emit_print(out, "uint64_t& lr");
              break;
            case 'r':
              emit_print(out, "PPCRegister& {}", reg);
              break;
            case 'f':
              if (reg == "fpscr")
                emit_print(out, "PPCFPSCRRegister& fpscr");
              else
                emit_print(out, "PPCRegister& {}", reg);
              break;
            case 'v':
              emit_print(out, "PPCVRegister& {}", reg);
              break;
          }
        }

        emit_println(out, ");\n");

        if (hookIt->second.jumpAddress != 0)
          labels.emplace(hookIt->second.jumpAddress);
        if (hookIt->second.jumpAddressOnTrue != 0)
          labels.emplace(hookIt->second.jumpAddressOnTrue);
        if (hookIt->second.jumpAddressOnFalse != 0)
          labels.emplace(hookIt->second.jumpAddressOnFalse);
      }
    }
  }

  // Collect labels from auto-detected jump tables
  for (const auto& jt : jumpTables()) {
    for (auto label : jt.targets) {
      labels.emplace(label);
    }
  }

  // --- Function name ---
  std::string name;
  if (base() == ctx.entryPoint) {
    name = "xstart";
  } else if (!name_.empty()) {
    name = name_;
  } else {
    name = fmt::format("sub_{:08X}", base());
  }
  name = ctx.namePrefix + name;

  // Function signature with weak/alias pattern
  emit_println(out, "DEFINE_REX_FUNC({}) {{", name);
  emit_println(out, "\tREX_FUNC_PROLOGUE();");
  if (ctx.config.callProbes)
    emit_println(out, "\tREX_PROBE_CALL(0x{:08X});", base());

  // --- Second pass: emit instruction code ---
  const JumpTable* activeJt = nullptr;
  bool allRecompiled = true;
  CSRState csrState = CSRState::Unknown;

  // fold_rodata_loads: GPRs whose 32-bit value is known from lis/li/addi/ori
  // along the straight-line path (cleared at labels, calls and hooks). A D-form
  // load through one of them from .rdata becomes the literal it would read.
  std::unordered_map<uint32_t, uint32_t> knownRegs;
  auto knownValue = [&](uint32_t reg) -> std::optional<uint32_t> {
    auto it = knownRegs.find(reg);
    return it == knownRegs.end() ? std::nullopt : std::optional<uint32_t>(it->second);
  };
  auto tryFoldRodataLoad = [&](const ppc_insn& insn, uint32_t address, std::string& out,
                               BuilderContext& regs) -> bool {
    uint32_t size = 0;
    switch (insn.opcode->id) {
      case PPC_INST_LFS: case PPC_INST_LWZ: case PPC_INST_LWA: size = 4; break;
      case PPC_INST_LFD: case PPC_INST_LD: size = 8; break;
      case PPC_INST_LHZ: size = 2; break;
      case PPC_INST_LBZ: size = 1; break;
      default: return false;
    }
    auto baseValue = knownValue(insn.operands[2]);
    if (!baseValue)
      return false;
    uint32_t target = *baseValue + static_cast<int32_t>(insn.operands[1]);
    const auto* section = ctx.binary.findSection(target);
    if (!section || section->name != ".rdata" || !section->contains(target + size - 1))
      return false;
    const uint8_t* bytes = section->translate(target);
    if (!bytes)
      return false;
    const char* dest = (insn.opcode->id == PPC_INST_LFS || insn.opcode->id == PPC_INST_LFD) ? "f" : "r";
    switch (insn.opcode->id) {
      case PPC_INST_LFS: {
        uint32_t bits = load_and_swap<uint32_t>(bytes);
        float f;
        std::memcpy(&f, &bits, sizeof f);
        double d = double(f);
        uint64_t dbits;
        std::memcpy(&dbits, &d, sizeof dbits);
        emit_println(out, "\t{}.u64 = 0x{:016X}ull;  // folded 0x{:08X}", regs.f(insn.operands[0]),
                     dbits, target);
        break;
      }
      case PPC_INST_LFD:
        emit_println(out, "\t{}.u64 = 0x{:016X}ull;  // folded 0x{:08X}", regs.f(insn.operands[0]),
                     load_and_swap<uint64_t>(bytes), target);
        break;
      case PPC_INST_LD:
        emit_println(out, "\t{}.u64 = 0x{:016X}ull;  // folded 0x{:08X}", regs.r(insn.operands[0]),
                     load_and_swap<uint64_t>(bytes), target);
        break;
      case PPC_INST_LWZ:
        emit_println(out, "\t{}.u64 = 0x{:08X}u;  // folded 0x{:08X}", regs.r(insn.operands[0]),
                     load_and_swap<uint32_t>(bytes), target);
        break;
      case PPC_INST_LWA:
        emit_println(out, "\t{}.s64 = int32_t(0x{:08X}u);  // folded 0x{:08X}",
                     regs.r(insn.operands[0]), load_and_swap<uint32_t>(bytes), target);
        break;
      case PPC_INST_LHZ:
        emit_println(out, "\t{}.u64 = 0x{:04X}u;  // folded 0x{:08X}", regs.r(insn.operands[0]),
                     load_and_swap<uint16_t>(bytes), target);
        break;
      case PPC_INST_LBZ:
        emit_println(out, "\t{}.u64 = 0x{:02X}u;  // folded 0x{:08X}", regs.r(insn.operands[0]),
                     bytes[0], target);
        break;
      default:
        return false;
    }
    (void)dest;
    ctx.fold(target, size);
    (void)address;
    return true;
  };
  // Keep knownRegs honest after an instruction: what it writes is unknown.
  auto trackRegisters = [&](const ppc_insn& insn) {
    const std::string_view name = insn.opcode->name;
    switch (insn.opcode->id) {
      case PPC_INST_LIS:
        knownRegs[insn.operands[0]] = static_cast<uint32_t>(insn.operands[1]) << 16;
        return;
      case PPC_INST_LI:
        knownRegs[insn.operands[0]] = static_cast<uint32_t>(static_cast<int32_t>(insn.operands[1]));
        return;
      case PPC_INST_ADDI:
      case PPC_INST_ADDIS:
      case PPC_INST_ORI: {
        auto baseValue = knownValue(insn.operands[1]);
        if (!baseValue) {
          knownRegs.erase(insn.operands[0]);
          return;
        }
        uint32_t imm = static_cast<uint32_t>(insn.operands[2]);
        if (insn.opcode->id == PPC_INST_ADDI)
          knownRegs[insn.operands[0]] = *baseValue + static_cast<int32_t>(imm);
        else if (insn.opcode->id == PPC_INST_ADDIS)
          knownRegs[insn.operands[0]] = *baseValue + (imm << 16);
        else
          knownRegs[insn.operands[0]] = *baseValue | (imm & 0xFFFF);
        return;
      }
      default:
        break;
    }
    if (name == "bl" || name == "bctrl" || name == "blrl" || name == "lmw" || name == "sc" ||
        name.starts_with("bcl") || name.starts_with("bdnzl") || name.starts_with("bdzl")) {
      knownRegs.clear();  // a call clobbers the volatile registers; lmw writes many
      return;
    }
    if (name.starts_with("st")) {
      // Stores write only their update register.
      if (name.ends_with("u") || name.ends_with("ux")) {
        knownRegs.erase(insn.operands[1]);
        knownRegs.erase(insn.operands[2]);
      }
      return;
    }
    if (name.starts_with("b"))
      return;  // branches write no GPR
    knownRegs.erase(insn.operands[0]);
    if (name.ends_with("u") || name.ends_with("ux")) {
      knownRegs.erase(insn.operands[1]);
      knownRegs.erase(insn.operands[2]);
    }
  };
  RecompilerLocalVariables localVariables;

  // NN-5.1 per-function specialisation. Reference copies and the save/restore
  // helpers are never specialised; a listed function that cannot keep its
  // registers in locals is emitted as usual (the reason goes to the stats).
  // A region oracle's candidate copy (orc_) is the optimized form and keeps
  // the specialisations.
  const bool specialize = allowSpecialization &&
                          (ctx.namePrefix.empty() || ctx.oracleCandidate) &&
                          !name_.starts_with("__");
  if (specialize && ctx.config.isRegisterLocalsFunction(static_cast<uint32_t>(base()))) {
    const SpecializeReason why = RegisterLocalsRefusal(*this, ctx, !resumeAddresses.empty());
    if (why == SpecializeReason::kApplied) {
      localVariables.allRegisters = true;
    } else {
      gRegisterLocalsStats[static_cast<size_t>(why)]++;
      REXCODEGEN_WARN("register_locals_functions: 0x{:08X} keeps its registers in ctx ({})",
                      base(), kSpecializeReasonNames[static_cast<size_t>(why)]);
    }
  }
  if (specialize) {
    localVariables.fixedFpHoist = ctx.config.isFixedFpModeFunction(static_cast<uint32_t>(base()));
    localVariables.singlePrecision =
        ctx.config.isSinglePrecisionFunction(static_cast<uint32_t>(base()));
  }
  // single_precision_functions: which FPRs hold a single-precision value,
  // along the straight-line path (cleared at labels, calls and hooks).
  auto trackSingles = [&](const ppc_insn& insn) {
    std::string_view name = insn.opcode->name;
    if (!name.empty() && name.back() == '.')
      name.remove_suffix(1);
    uint32_t& singles = localVariables.singleFprs;
    auto set = [&](uint32_t reg, bool single) {
      if (reg >= 32)
        return;
      if (single)
        singles |= 1u << reg;
      else
        singles &= ~(1u << reg);
    };
    auto isSingle = [&](uint32_t reg) { return reg < 32 && ((singles >> reg) & 1); };
    const uint32_t word = insn.instruction;
    const uint32_t op = word >> 26;
    // Any linked branch (a call) or system call may change any FPR.
    if (((op == 16 || op == 18 || op == 19) && (word & 1)) || op == 17) {
      singles = 0;
      return;
    }
    if (name == "lfs" || name == "lfsx" || name == "lfsu" || name == "lfsux" || name == "fadds" ||
        name == "fsubs" || name == "fmuls" || name == "fdivs" || name == "fmadds" ||
        name == "fmsubs" || name == "fnmadds" || name == "fnmsubs" || name == "fres" ||
        name == "frsp" || name == "fsqrts") {
      set(insn.operands[0], true);
    } else if (name == "fmr" || name == "fneg" || name == "fabs" || name == "fnabs") {
      set(insn.operands[0], isSingle(insn.operands[1]));
    } else if (name == "fsel") {
      set(insn.operands[0], isSingle(insn.operands[2]) && isSingle(insn.operands[3]));
    } else if ((name.starts_with("f") && !name.starts_with("fcmp")) || name.starts_with("lfd") ||
               name == "mffs") {
      set(insn.operands[0], false);
    }
  };

  // host_local_register_saves: keep the prologue's saved GPRs in host locals
  // when the save-area scan finds nothing else that needs the stack slots.
  if (ctx.config.hostLocalRegisterSaves && !name_.starts_with("__")) {
    const HostSavePlan plan = ScanHostLocalSaves(*this, ctx);
    // host_local_lr: keep the LR save slot in a host local as well when the
    // LR-slot scan finds nothing else that reads or writes it.
    if (ctx.config.hostLocalLr) {
      HostLrPlan lrPlan = ScanHostLocalLr(*this, ctx, plan);
      if (lrPlan.reason != HostLrReason::kNoSlot && ctx.namePrefix.empty()) {
        gHostLrStats[static_cast<size_t>(lrPlan.reason)]++;
        if (lrPlan.reason == HostLrReason::kTransformed) {
          (lrPlan.store ? gHostLrExplicitForm : gHostLrHelperForm)++;
          gHostLrLoadsRewritten += lrPlan.loads.size();
        } else {
          REXCODEGEN_DEBUG("host_local_lr: 0x{:08X} keeps the LR slot ({} at 0x{:08X})", base(),
                           kHostLrReasonNames[static_cast<size_t>(lrPlan.reason)],
                           lrPlan.address);
        }
      }
      if (lrPlan.reason == HostLrReason::kTransformed) {
        localVariables.hostLr = true;
        localVariables.hostLrStore = lrPlan.store;
        localVariables.hostLrLoads = std::move(lrPlan.loads);
      }
    }
    if (plan.reason != HostSaveReason::kNoSave && ctx.namePrefix.empty()) {
      gHostSaveStats[static_cast<size_t>(plan.reason)]++;
      if (plan.reason != HostSaveReason::kTransformed)
        REXCODEGEN_DEBUG("host_local_register_saves: 0x{:08X} keeps the helpers ({} at 0x{:08X})",
                         base(), kHostSaveReasonNames[static_cast<size_t>(plan.reason)],
                         plan.address);
    }
    if (plan.reason == HostSaveReason::kTransformed)
      localVariables.hostSaveFirst = static_cast<uint8_t>(plan.first);
  }

  // Local map for late-detected jump tables (can't mutate const config)
  std::unordered_map<uint32_t, JumpTable> lateJumpTables;

  std::string body;
  body.reserve(4096);

  if (!resumeAddresses.empty()) {
    emit_println(body, "\tconst uint32_t rex_dispatch_address = ctx.dispatch_address;");
    emit_println(body, "\tctx.dispatch_address = 0;");
    emit_println(body, "\tswitch (rex_dispatch_address) {{");
    emit_println(body, "\t\tcase 0:");
    emit_println(body, "\t\tcase 0x{:08X}: break;", base());
    for (uint32_t address : resumeAddresses) {
      emit_println(body, "\t\tcase 0x{:08X}: goto loc_{:X};", address, address);
    }
    emit_println(body, "\t\tdefault: break;");
    emit_println(body, "\t}}\n");
  } else if (ctx.config.interiorResume) {
    emit_println(body, "\tctx.dispatch_address = 0;");
  }

  ppc_insn insn;
  std::unordered_set<size_t> emittedLabels;

  for (const auto& block : blocks()) {
    auto blockBase = block.base;
    auto blockEnd = block.end();
    auto* data = reinterpret_cast<const uint32_t*>(ctx.binary.translate(block.base));
    if (!data) {
      REXCODEGEN_WARN("Block 0x{:08X} in function 0x{:08X} has no mapped data - skipping",
                      block.base, base());
      continue;
    }

    while (blockBase < blockEnd) {
      // Only emit each label once
      if (labels.find(blockBase) != labels.end() && emittedLabels.insert(blockBase).second) {
        emit_println(body, "loc_{:X}:", blockBase);
        csrState = CSRState::Unknown;
        knownRegs.clear();
        localVariables.singleFprs = 0;
      }

      // Look up switch table for this address
      activeJt = nullptr;
      auto stIt = ctx.config.switchTables.find(blockBase);
      if (stIt != ctx.config.switchTables.end()) {
        activeJt = &stIt->second;
      } else {
        auto lateIt = lateJumpTables.find(blockBase);
        if (lateIt != lateJumpTables.end()) {
          activeJt = &lateIt->second;
        }
      }

      Disassemble(data, 4, blockBase, insn);

      if (insn.opcode == nullptr) {
        emit_println(body, "\t// {}", insn.op_str);
        if (*data != 0)
          REXCODEGEN_WARN("Unable to decode instruction {:X} at {:X}", *data, blockBase);
      } else {
        // Late jump table detection for bctr
        if (insn.opcode->id == PPC_INST_BCTR && !activeJt) {
          bool is_switch_pattern = false;
          constexpr uint32_t MTCTR_MASK = 0xFC1FFFFF;
          constexpr uint32_t MTCTR_OPCODE = 0x7C0003A6;
          constexpr uint32_t NOP = 0x60000000;

          for (int i = 1; i <= 3 && !is_switch_pattern; i++) {
            uint32_t prev_insn = load_and_swap<uint32_t>(data - i);
            if ((prev_insn & MTCTR_MASK) == MTCTR_OPCODE) {
              is_switch_pattern = true;
              for (int j = 1; j < i; j++) {
                if (load_and_swap<uint32_t>(data - j) != NOP) {
                  is_switch_pattern = false;
                  break;
                }
              }
            } else if (prev_insn != NOP) {
              break;
            }
          }

          if (is_switch_pattern) {
            FunctionScanner scanner(ctx.binary);
            auto jt_opt = scanner.detect_jump_table(blockBase);
            if (jt_opt.has_value()) {
              lateJumpTables.emplace(blockBase, std::move(*jt_opt));
              activeJt = &lateJumpTables.at(blockBase);
              for (auto label : activeJt->targets) {
                labels.emplace(label);
              }
              REXCODEGEN_TRACE("Late-detected jump table at 0x{:08X} with {} entries", blockBase,
                               activeJt->targets.size());
            }
          }
        }

        // A line directive names the guest byte offset, so the debug line
        // table maps host code back to guest addresses whatever the C++ looks
        // like. Instructions are 4 apart, so an instruction's lines stay
        // distinct up to four C++ lines; longer expansions bleed into the
        // next instruction's numbers.
        if (ctx.config.guestLineDirectives) {
          emit_println(body, "#line {} \"{}.guest\"", blockBase - ctx.binary.baseAddress() + 1,
                       ctx.config.projectName);
        }

        // Emit comment with instruction disassembly
        if (ctx.config.disassemblyComments)
          emit_println(body, "\t// {} {}", insn.opcode->name, insn.op_str);

        // Check for mid-asm hook BEFORE instruction
        auto hookIt = ctx.config.midAsmHooks.find(blockBase);
        bool hasHookBefore =
            (hookIt != ctx.config.midAsmHooks.end() && !hookIt->second.afterInstruction);

        // Dispatch instruction to builder
        int id = insn.opcode->id;
        BuilderContext builderCtx{body,           ctx,      *this,   insn, blockBase, data,
                                  localVariables, csrState, activeJt};

        if (hasHookBefore) {
          builderCtx.emit_mid_asm_hook();
          knownRegs.clear();  // a hook may write any register
          localVariables.singleFprs = 0;
        }

        const size_t bodyBefore = body.size();
        bool folded = ctx.config.foldRodataLoads && !localVariables.isHostLrLoad(blockBase) &&
                      tryFoldRodataLoad(insn, blockBase, body, builderCtx);
        if (!folded && !DispatchInstruction(id, builderCtx)) {
          REXCODEGEN_WARN("Unrecognized instruction at 0x{:X}: {}", blockBase, insn.opcode->name);
          allRecompiled = false;
        }
        if (ctx.config.foldRodataLoads)
          trackRegisters(insn);
        if (localVariables.singlePrecision)
          trackSingles(insn);
        if (ctx.referenceBlocked) {
          // A reference copy must be rolled back by its store log alone: no
          // stores outside the macros, no atomics, no calls, no global state.
          // A region oracle copy may call the region's own copies (they log
          // too) and stops at every other call (REX_ORACLE_OPAQUE, an
          // indirect call included), but not at a mid-asm hook, whose host
          // code would read and write the guest behind the log.
          const std::string_view mnemonic = insn.opcode->name;
          const std::string_view added(body.data() + bodyBefore, body.size() - bodyBefore);
          const bool oracle = ctx.oracleMembers != nullptr;
          auto foreignCall = [&] {
            if (!oracle)
              return added.find("(ctx, base);") != std::string_view::npos ||
                     added.find("REX_TAIL_CALL(") != std::string_view::npos ||
                     added.find("REX_CALL_INDIRECT") != std::string_view::npos;
            for (size_t at = added.find("(ctx, base);"); at != std::string_view::npos;
                 at = added.find("(ctx, base);", at + 1)) {
              size_t start = at;
              while (start > 0 && (std::isalnum(static_cast<unsigned char>(added[start - 1])) ||
                                   added[start - 1] == '_'))
                --start;
              if (!added.substr(start, at - start).starts_with(ctx.oraclePrefix))
                return true;
            }
            return false;
          };
          if (oracle && hookIt != ctx.config.midAsmHooks.end())
            *ctx.referenceBlocked = true;
          if (mnemonic.starts_with("stwcx") ||
              mnemonic.starts_with("stdcx") || mnemonic.starts_with("dcbz") || mnemonic == "sc" ||
              mnemonic.starts_with("mtmsr") || added.find("REX_MM_STORE") != std::string_view::npos || foreignCall() ||
              added.find("GLOBAL_LOCK") != std::string_view::npos ||
              added.find("ppc_setjmp") != std::string_view::npos ||
              added.find("ppc_longjmp") != std::string_view::npos ||
              added.find("simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR") !=
                  std::string_view::npos ||
              added.find("memset(") != std::string_view::npos) {
            *ctx.referenceBlocked = true;
          }
        }

        // Check for mid-asm hook AFTER instruction
        if (hookIt != ctx.config.midAsmHooks.end() && hookIt->second.afterInstruction) {
          builderCtx.emit_mid_asm_hook();
          localVariables.singleFprs = 0;
        }
      }

      blockBase += 4;
      ++data;
    }
  }

  // A configured split may end immediately before another callable entry
  // without an explicit guest branch. PowerPC execution naturally falls
  // through in that case, so preserve it as a host tail call. This is limited
  // to an exact registered boundary and a final non-control-flow instruction;
  // explicit branches and returns already emit their own transfer.
  if (!blocks().empty() && blocks().back().end() == end()) {
    const auto* fallthroughTarget = ctx.graph.getFunction(end());
    const auto* lastInstructionData = ctx.binary.translate(end() - 4);
    if (fallthroughTarget && fallthroughTarget != this && lastInstructionData) {
      const uint32_t lastInstruction = load_and_swap<uint32_t>(lastInstructionData);
      const uint32_t opcode = PPC_OP(lastInstruction);
      const bool hasExplicitTransfer =
          (opcode == PPC_OP_B || opcode == PPC_OP_CTR) && !PPC_BL(lastInstruction);
      if (!hasExplicitTransfer && ctx.oracleMembers &&
          (!ctx.regionRealCalls || ctx.oracleMembers->count(fallthroughTarget->base()))) {
        // A region copy falls through into the next function's copy, or
        // leaves the region there (a native region copy leaves it for real,
        // below).
        if (ctx.oracleMembers->count(fallthroughTarget->base())) {
          ctx.reference(ctx.oraclePrefix + fallthroughTarget->name());
          emit_println(body, "\tREX_TAIL_CALL({}{});", ctx.oraclePrefix,
                       fallthroughTarget->name());
        } else {
          emit_println(body, "\tREX_ORACLE_OPAQUE(ctx, base, 0x{:08X}u, 0x{:08X}u);",
                       fallthroughTarget->base(), end() - 4);
        }
      } else if (!hasExplicitTransfer) {
        ctx.reference(fallthroughTarget->name());
        emit_println(body, "\tREX_TAIL_CALL({});", fallthroughTarget->name());
      }
    }
  }

  // --- Close function body (or SEH try block) ---
  bool generateSeh = sehInfo && !sehInfo->scopes.empty() && ctx.config.generateExceptionHandlers;
  if (generateSeh) {
    emit_println(body, "\t\t}} SEH_CATCH_ALL {{");
    emit_println(body, "\t\t\tREXLOG_WARN(\"SEH exception caught in sub_{:08X}\");", base());

    if (sehInfo->frameSize > 0) {
      emit_println(body, "\t\t\tctx.r12.s64 = ctx.r31.s64 + {};  // Establisher frame pointer",
                   sehInfo->frameSize);
    }

    for (auto it = sehInfo->scopes.rbegin(); it != sehInfo->scopes.rend(); ++it) {
      const auto& scope = *it;
      if (scope.filter == 0 && scope.handler != 0) {
        ctx.reference(fmt::format("sub_{:08X}", scope.handler));
        emit_println(body, "\t\t\tsub_{:08X}(ctx, base);  // __finally handler", scope.handler);
      }
    }

    if (sehInfo->restoreHelper != 0) {
      auto* restoreFn = ctx.graph.getFunction(sehInfo->restoreHelper);
      if (restoreFn && !restoreFn->name().empty()) {
        ctx.reference(restoreFn->name());
        emit_println(body, "\t\t\t{}(ctx, base);  // Restore caller registers", restoreFn->name());
      }
    }

    emit_println(body, "\t\t\tSEH_RETHROW;");
    emit_println(body, "\t\t}} SEH_END");
    emit_println(body, "\t}}\n");
  } else {
    emit_println(body, "}}\n");
  }

  body = PropagateFlushMode(body);

  // register_locals_functions: a form that copies or restores all of ctx
  // (setjmp, longjmp, SEH) cannot see the locals; emit this one as usual.
  if (localVariables.allRegisters && HasWholeContextForms(body)) {
    gRegisterLocalsStats[static_cast<size_t>(SpecializeReason::kSetJmpOrSeh)]++;
    REXCODEGEN_WARN("register_locals_functions: 0x{:08X} keeps its registers in ctx ({})", base(),
                    kSpecializeReasonNames[static_cast<size_t>(SpecializeReason::kSetJmpOrSeh)]);
    return emitCpp(ctx, false);
  }
  if (localVariables.allRegisters)
    gRegisterLocalsStats[static_cast<size_t>(SpecializeReason::kApplied)]++;

  // register_locals_functions with flush-mode toggles: with the guest's FP
  // values in locals, clang may move arithmetic across a host FP control
  // write (it does not model the FP environment), so a scalar operation can
  // run with flush-to-zero on and lose a denormal that the ctx-backed body
  // keeps. Only fixed FP mode, where the toggles do nothing, is safe; in the
  // other mode the function runs an unspecialised copy (`ieee_sub_X`),
  // emitted just before it. The offline equivalence check found this
  // (denormals flushed in IEEE-toggle mode, bit-exact in fixed mode).
  std::string ieeeCopy;
  std::string ieeeName;
  if (localVariables.allRegisters &&
      (body.find("ctx.fpscr.enableFlushMode") != std::string::npos ||
       body.find("ctx.fpscr.disableFlushMode") != std::string::npos)) {
    EmitContext ieeeCtx = ctx;
    ieeeCtx.namePrefix = "ieee_" + ctx.namePrefix;
    ieeeCtx.referenceBlocked = nullptr;
    ieeeCopy = emitCpp(ieeeCtx, false);
    ieeeName = "ieee_" + name;
    gIeeeFallbacks++;
  }
  if (localVariables.singlePrecision) {
    size_t fmas = 0;
    for (std::string_view form : {"double(std::fma(float(", "double(-std::fma(float("}) {
      for (size_t at = body.find(form); at != std::string::npos; at = body.find(form, at + 1))
        ++fmas;
    }
    gSingleFunctions++;
    gSingleFmas += fmas;
  }

  // CR fields kept in locals are synchronized with ctx at every boundary.
  // Non-volatile GPR, FPR and VR locals (non_volatile_as_local) join the full
  // synchronization (a hook given ctx, the setjmp copy, longjmp and the SEH
  // catch block), and the ones the function writes are published at calls
  // and returns below.
  bool anyCrLocal = false;
  for (size_t i = 0; i < 8; i++)
    anyCrLocal |= localVariables.cr[i];
  bool anyNvLocal = false;
  for (size_t i = 14; i < 32; i++)
    anyNvLocal |= localVariables.r[i] || localVariables.f[i] || localVariables.v[i];
  for (size_t i = 64; i < 128; i++)
    anyNvLocal |= localVariables.v[i];
  // register_locals_functions (NN-5.1): every register is a local, so the
  // synchronization covers the volatile ones as well: all locals in the full
  // spill and reload, every written one published at calls, tail calls and
  // returns, and the volatile ones read back after each call, where a callee
  // may have left new values (callers in this title read some, NL-0.1).
  const bool all = localVariables.allRegisters;
  const size_t firstGpr = all ? 0 : 14;
  auto vectorInSync = [&](size_t i) {
    return all || !(i < 14 || (i >= 32 && i < 64));
  };
  if (all)
    anyNvLocal = true;
  std::string fallOffPublish;
  if (anyCrLocal || anyNvLocal) {
    std::string spill = "\t";
    std::string spill_nv;
    std::string reload = "\t";
    for (size_t i = 0; i < 8; i++) {
      if (!localVariables.cr[i])
        continue;
      spill += fmt::format("ctx.cr{0} = cr{0}; ", i);
      reload += fmt::format("cr{0} = ctx.cr{0}; ", i);
      if (i >= 2 && i <= 4) {
        spill_nv += fmt::format("ctx.cr{0} = cr{0}; ", i);
      }
    }
    for (size_t i = firstGpr; i < 32; i++) {
      if (localVariables.r[i]) {
        spill += fmt::format("ctx.r{0} = r{0}; ", i);
        reload += fmt::format("r{0} = ctx.r{0}; ", i);
      }
      if (localVariables.f[i]) {
        spill += fmt::format("ctx.f{0} = f{0}; ", i);
        reload += fmt::format("f{0} = ctx.f{0}; ", i);
      }
    }
    for (size_t i = 0; i < 128; i++) {
      if (!vectorInSync(i))
        continue;
      if (localVariables.v[i]) {
        spill += fmt::format("ctx.v{0} = v{0}; ", i);
        reload += fmt::format("v{0} = ctx.v{0}; ", i);
      }
    }
    // The non-volatile locals this function writes are published before every
    // call, tail call and return, so ctx holds the register file at every
    // function boundary: a callee may read any register through ctx (its own
    // entry loads, a share_registers function, a host hook), and after the
    // inlined restore the publish at return hands the caller its values back.
    // Locals the function never writes keep their entry value, which ctx still
    // holds, since every callee restores what it publishes.
    const std::string crSpill = spill_nv;
    const std::vector<std::string> writtenRegisters =
        all ? WrittenAllRegisters(body) : WrittenLocalRegisters(body);
    for (const std::string& reg : writtenRegisters) {
      spill_nv += fmt::format("ctx.{0} = {0}; ", reg);
    }
    if (!spill_nv.empty()) {
      spill_nv = "\t" + spill_nv;
    }
    // Optionally the non-volatile locals are read back from ctx after every
    // call, so a callee that leaves a changed value in one (hand-written code)
    // is seen as it would be on the console. Off by default: FH1's race and
    // boot run without it, and it costs a load per local per call.
    std::string reload_nv;
    for (size_t i = 14; ctx.config.nonVolatileReloadAfterCall && i < 32; i++) {
      if (localVariables.r[i])
        reload_nv += fmt::format("r{0} = ctx.r{0}; ", i);
      if (localVariables.f[i])
        reload_nv += fmt::format("f{0} = ctx.f{0}; ", i);
    }
    for (size_t i = 14; ctx.config.nonVolatileReloadAfterCall && i < 128; i++) {
      if ((i >= 32 && i < 64) || !localVariables.v[i])
        continue;
      reload_nv += fmt::format("v{0} = ctx.v{0}; ", i);
    }
    // register_locals_functions: the volatile locals (r0, r2-r12, f0-f13,
    // v0-v13, v32-v63) come back from ctx after every call. r1 and r13 are
    // preserved by every callee.
    for (size_t i = 0; all && i < 14; i++) {
      if (localVariables.r[i] && i != 1 && i != 13)
        reload_nv += fmt::format("r{0} = ctx.r{0}; ", i);
      if (localVariables.f[i])
        reload_nv += fmt::format("f{0} = ctx.f{0}; ", i);
    }
    for (size_t i = 0; all && i < 64; i++) {
      if (localVariables.v[i] && (i < 14 || i >= 32))
        reload_nv += fmt::format("v{0} = ctx.v{0}; ", i);
    }
    if (!reload_nv.empty()) {
      reload_nv = "\t" + reload_nv;
    }
    if (all) {
      size_t stores = 0;
      body = SyncAllRegisterLocals(body, writtenRegisters, crSpill, reload_nv, stores);
      fallOffPublish = spill_nv;
    } else {
      body = SyncLocalCrRegisters(body, spill, spill_nv, reload, reload_nv);
    }
  }
  // A body that runs off its end (no return) publishes too; one whose last
  // statement leaves the function needs nothing there.
  if (!fallOffPublish.empty()) {
    const size_t close = body.rfind("}\n");
    if (close != std::string::npos) {
      std::string_view before(body.data(), close);
      while (!before.empty() && (before.back() == '\n' || before.back() == '\t'))
        before.remove_suffix(1);
      const size_t lineStart = before.rfind('\n');
      std::string_view last = before.substr(lineStart == std::string_view::npos ? 0 : lineStart + 1);
      last.remove_prefix(std::min(last.find_first_not_of('\t'), last.size()));
      const bool leaves = last == "return;" || last.starts_with("REX_TAIL_CALL(") ||
                          last.starts_with("[[clang::musttail]]");
      if (!leaves)
        body.insert(close, fallOffPublish + "\n");
    }
  }
  // fixed_fp_mode_functions: every toggle tests the mode read at entry.
  size_t fixedFpSites = 0;
  if (!ieeeName.empty()) {
    fixedFpSites = HoistFixedFpMode(body, true);
    gFixedFpFunctions++;
    gFixedFpSites += fixedFpSites;
  } else if (localVariables.fixedFpHoist) {
    fixedFpSites = HoistFixedFpMode(body, false);
    if (fixedFpSites) {
      gFixedFpFunctions++;
      gFixedFpSites += fixedFpSites;
    }
  }

  // --- Emit local variable declarations, then body ---
  if (localVariables.ctr)
    emit_println(out, "\tPPCRegister ctr{{}};");
  if (localVariables.xer)
    emit_println(out, "\tPPCXERRegister xer{{}};");
  if (localVariables.reserved)
    emit_println(out, "\tPPCRegister reserved{{}};");

  for (size_t i = 0; i < 8; i++) {
    if (localVariables.cr[i])
      emit_println(out, "\tPPCCRRegister cr{0} = ctx.cr{0};", i);
  }

  // A non-volatile local starts as the register's value in ctx, so until the
  // function writes it the local holds what the caller left there, exactly as
  // the register would: a function that reads one before writing it still
  // works, and the locals a function publishes before calling a
  // share_registers function are always the right values. A non-argument
  // volatile local is dead on entry and starts at zero.
  // With register_locals_functions every local starts from ctx: an argument
  // or a value a caller left in a volatile is read before it is written.
  for (size_t i = 0; i < 32; i++) {
    if (!localVariables.r[i])
      continue;
    if (i >= 14 || all)
      emit_println(out, "\tPPCRegister r{0} = ctx.r{0};", i);
    else
      emit_println(out, "\tPPCRegister r{}{{}};", i);
  }

  for (size_t i = 0; i < 32; i++) {
    if (!localVariables.f[i])
      continue;
    if (i >= 14 || all)
      emit_println(out, "\tPPCRegister f{0} = ctx.f{0};", i);
    else
      emit_println(out, "\tPPCRegister f{}{{}};", i);
  }

  for (size_t i = 0; i < 128; i++) {
    if (!localVariables.v[i])
      continue;
    if ((i >= 14 && i < 32) || i >= 64 || all)
      emit_println(out, "\tPPCVRegister v{0} = ctx.v{0};", i);
    else
      emit_println(out, "\tPPCVRegister v{}{{}};", i);
  }

  if (localVariables.env)
    emit_println(out, "\tPPCContext env{{}};");
  if (localVariables.temp)
    emit_println(out, "\tPPCRegister temp{{}};");
  if (localVariables.v_temp)
    emit_println(out, "\tPPCVRegister vTemp{{}};");
  if (localVariables.ea)
    emit_println(out, "\tuint32_t ea{{}};");
  for (int i = localVariables.hostSaveFirst ? localVariables.hostSaveFirst : 32; i < 32; ++i)
    emit_println(out, "\tuint64_t rex_save_r{}{{}};", i);
  if (localVariables.hostLr)
    emit_println(out, "\tuint32_t rex_saved_lr{{}};");
  // fixed_fp_mode_functions: the fixed FP mode switch, read once. In fixed
  // mode nothing sets flush-to-zero, so one clear of a bit left from before
  // the switch was turned on makes every toggle here a no-op.
  if (!ieeeName.empty()) {
    // Before the locals are read, so the IEEE copy starts from ctx alone.
    std::string dispatch = fmt::format(
        "\tif (!PPCFPSCRRegister::FixedMode()) [[unlikely]] {{ REX_TAIL_CALL({}); }}\n"
        "\tctx.fpscr.disableFlushMode();\n",
        ieeeName);
    const size_t prologue = out.find("\tREX_FUNC_PROLOGUE();\n");
    if (prologue != std::string::npos)
      out.insert(prologue + std::string_view("\tREX_FUNC_PROLOGUE();\n").size(), dispatch);
  } else if (fixedFpSites) {
    emit_println(out, "\tconst bool rex_fixed_fp = PPCFPSCRRegister::FixedMode();");
    emit_println(out, "\tif (rex_fixed_fp) ctx.fpscr.disableFlushMode();");
  }

  // If SEH, emit SEH_TRY and indent body
  if (generateSeh) {
    emit_println(out, "\tSEH_TRY {{");
    std::string indentedBody;
    indentedBody.reserve(body.size() + body.size() / 20);
    for (size_t i = 0; i < body.size(); ++i) {
      indentedBody += body[i];
      if (body[i] == '\n' && i + 1 < body.size() && body[i + 1] == '\t') {
        indentedBody += '\t';
      }
    }
    out += indentedBody;
  } else {
    out += body;
  }

  if (!ieeeCopy.empty())
    return ieeeCopy + "\n" + out;
  return out;
}

void FunctionGraph::addCodeBuffer(uint32_t baseAddress, const uint8_t* data, size_t size) {
  assert_always("FunctionGraph::addCodeBuffer not implemented. Use Recompiler with builders");
  return;
  // CodeBuffer buffer;
  // buffer.baseAddress = baseAddress;
  // buffer.data.assign(data, data + size);
  // codeBuffers_.push_back(std::move(buffer));
  // REXCODEGEN_DEBUG("FunctionGraph: added code buffer 0x{:08X}-0x{:08X} ({} bytes)",
  //                 baseAddress, baseAddress + static_cast<uint32_t>(size), size);
}

const uint8_t* FunctionGraph::translateCode(uint32_t addr) const {
  assert_always("FunctionGraph::translateCode not implemented. Use Recompiler with builders");
  return nullptr;
  // for (const auto& buffer : codeBuffers_) {
  //     if (buffer.contains(addr)) {
  //         return buffer.translate(addr);
  //     }
  // }
  // return nullptr;
}

void FunctionGraph::updateFunctionCodePointers() {
  // size_t updated = 0;
  // for (auto& [base, node] : functions_) {
  //     const uint8_t* code = translateCode(base);
  //     if (code) {
  //         node->setCode(code);
  //         updated++;
  //     } else {
  //         REXCODEGEN_WARN("FunctionGraph: no code buffer for function 0x{:08X}", base);
  //     }
  // }
  // REXCODEGEN_DEBUG("FunctionGraph: updated code pointers for {} functions", updated);
}

//=============================================================================
// FunctionGraph - Function Management
//=============================================================================

FunctionNode* FunctionGraph::addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                                         bool hasXrefs) {
  // Check for existing function at this address
  auto it = functions_.find(base);
  if (it != functions_.end()) {
    FunctionNode* existing = it->second.get();

    // Higher authority wins
    if (existing->authority() >= authority) {
      REXCODEGEN_TRACE(
          "FunctionGraph: ignoring add of 0x{:08X} ({}) - existing has higher authority ({})", base,
          AuthorityName(authority), AuthorityName(existing->authority()));
      return existing;
    }

    // Replace with higher authority
    REXCODEGEN_DEBUG("FunctionGraph: replacing 0x{:08X} ({}) with ({})", base,
                     AuthorityName(existing->authority()), AuthorityName(authority));
  }

  // Create new node
  auto node = std::make_unique<FunctionNode>(base, size, authority);
  FunctionNode* nodePtr = node.get();
  functions_[base] = std::move(node);
  functionsByBase_[base] = nodePtr;

  // Track xrefs for merge eligibility
  functionHasXrefs_[base] = hasXrefs;

  // Notify all PENDING functions
  notifyFunctionAdded(nodePtr);

  return nodePtr;
}

FunctionNode* FunctionGraph::addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                                         std::string_view name, bool hasXrefs) {
  auto* node = addFunction(base, size, authority, hasXrefs);
  if (node && !name.empty()) {
    node->setName(std::string(name));
  }
  return node;
}

FunctionNode* FunctionGraph::addImportFunction(uint32_t address, std::string_view resolvedName) {
  // Add as function with IMPORT authority
  auto* node = addFunction(address, 4, FunctionAuthority::IMPORT, resolvedName, true);
  REXCODEGEN_TRACE("FunctionGraph: added import function 0x{:08X} -> {}", address, resolvedName);
  return node;
}

FunctionNode* FunctionGraph::getFunction(uint32_t entryPoint) {
  auto it = functions_.find(entryPoint);
  return it != functions_.end() ? it->second.get() : nullptr;
}

const FunctionNode* FunctionGraph::getFunction(uint32_t entryPoint) const {
  auto it = functions_.find(entryPoint);
  return it != functions_.end() ? it->second.get() : nullptr;
}

bool FunctionGraph::removeFunction(uint32_t entryPoint) {
  auto it = functions_.find(entryPoint);
  if (it == functions_.end()) {
    return false;
  }
  REXCODEGEN_TRACE("FunctionGraph: removing absorbed function 0x{:08X}", entryPoint);
  functionsByBase_.erase(entryPoint);
  functions_.erase(it);
  functionHasXrefs_.erase(entryPoint);  // Clean up xref tracking
  return true;
}

FunctionNode* FunctionGraph::getFunctionContaining(uint32_t addr) {
  // O(log f) lookup via sorted base index: find last function with base <= addr
  auto it = functionsByBase_.upper_bound(addr);
  if (it != functionsByBase_.begin()) {
    --it;
    if (it->second->containsAddress(addr)) {
      return it->second;
    }
  }
  return nullptr;
}

const FunctionNode* FunctionGraph::getFunctionContaining(uint32_t addr) const {
  auto it = functionsByBase_.upper_bound(addr);
  if (it != functionsByBase_.begin()) {
    --it;
    if (it->second->containsAddress(addr)) {
      return it->second;
    }
  }
  return nullptr;
}

bool FunctionGraph::isEntryPoint(uint32_t addr) const {
  return functions_.contains(addr);
}

bool FunctionGraph::isImport(uint32_t addr) const {
  auto it = functions_.find(addr);
  return it != functions_.end() && it->second->authority() == FunctionAuthority::IMPORT;
}

std::vector<FunctionNode*> FunctionGraph::getPendingFunctions() {
  std::vector<FunctionNode*> result;
  for (auto& [base, node] : functions_) {
    if (node->isPending()) {
      result.push_back(node.get());
    }
  }
  return result;
}

std::vector<FunctionNode*> FunctionGraph::getSealedFunctions() {
  std::vector<FunctionNode*> result;
  for (auto& [base, node] : functions_) {
    if (node->isSealed()) {
      result.push_back(node.get());
    }
  }
  return result;
}

size_t FunctionGraph::pendingCount() const {
  size_t count = 0;
  for (const auto& [base, node] : functions_) {
    if (node->isPending())
      ++count;
  }
  return count;
}

size_t FunctionGraph::sealedCount() const {
  size_t count = 0;
  for (const auto& [base, node] : functions_) {
    if (node->isSealed())
      ++count;
  }
  return count;
}

//=============================================================================
// Function Setup (called during Discover phase)
//=============================================================================

void FunctionGraph::setFunctionName(uint32_t entry, std::string name) {
  if (auto* node = getFunction(entry)) {
    node->setName(std::move(name));
  }
}

void FunctionGraph::setFunctionHasExceptionHandler(uint32_t entry, bool val) {
  if (auto* node = getFunction(entry)) {
    node->setHasExceptionHandler(val);
  }
}

void FunctionGraph::setFunctionExceptionInfo(uint32_t entry, ExceptionInfo info) {
  if (auto* node = getFunction(entry)) {
    node->setExceptionInfo(std::move(info));
  }
}

void FunctionGraph::addBlockToFunction(uint32_t entry, Block block) {
  if (auto* node = getFunction(entry)) {
    node->addBlock(block);
  }
}

void FunctionGraph::addLabelToFunction(uint32_t entry, uint32_t label) {
  if (auto* node = getFunction(entry)) {
    node->addLabel(label);
  }
}

void FunctionGraph::addCallToFunction(uint32_t entry, uint32_t site, CallTarget target) {
  if (auto* node = getFunction(entry)) {
    node->addCall(site, std::move(target));
  }
}

void FunctionGraph::addTailCallToFunction(uint32_t entry, uint32_t site, CallTarget target) {
  if (auto* node = getFunction(entry)) {
    node->addTailCall(site, std::move(target));
  }
}

void FunctionGraph::addJumpTableToFunction(uint32_t entry, JumpTable jt) {
  if (auto* node = getFunction(entry)) {
    node->addJumpTable(std::move(jt));
  }
}

void FunctionGraph::addUnresolvedJumpToFunction(uint32_t entry, uint32_t site, uint32_t target,
                                                bool isCall, bool conditional) {
  auto* node = getFunction(entry);
  if (!node)
    return;

  // A configured chunk is both a callable entry and an internal block of its
  // owning function. Keep an owning function's branch unresolved until the
  // merge phase, where internal labels are deliberately checked before
  // function entries. Other callers still resolve the chunk as a tail call.
  if (!isCall) {
    const bool isOwnedChunk = std::ranges::any_of(chunks_, [&](const ChunkInfo& chunk) {
      return chunk.base == target && chunk.parent == entry;
    });
    if (isOwnedChunk) {
      node->addUnresolvedJump(site, target, false, conditional);
      REXCODEGEN_TRACE(
          "FunctionGraph: deferred owned-chunk jump 0x{:08X}->0x{:08X} in function 0x{:08X}",
          site, target, entry);
      return;
    }
  }

  // Try immediate resolution against existing functions/imports
  if (auto* targetFn = getFunction(target)) {
    // Target is a known function - resolve as call or tail call
    if (isCall) {
      node->addCall(site, CallTarget::function(targetFn));
    } else {
      node->addTailCall(site, CallTarget::function(targetFn));
    }
    REXCODEGEN_TRACE("FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to {}", site,
                     target, isCall ? "call" : "tail call", targetFn->name());
    return;
  }

  if (isImport(target)) {
    // Target is an import - resolve as call or tail call to import
    auto* importNode = getFunction(target);
    const std::string& importName = importNode->name();
    if (isCall) {
      node->addCall(site, CallTarget::import(target, importName));
    } else {
      node->addTailCall(site, CallTarget::import(target, importName));
    }
    REXCODEGEN_TRACE("FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to import {}",
                     site, target, isCall ? "call" : "tail call", importName);
    return;
  }

  // Not resolvable yet - add as unresolved
  node->addUnresolvedJump(site, target, isCall, conditional);
  REXCODEGEN_TRACE("FunctionGraph: added unresolved {} 0x{:08X}->0x{:08X} to function 0x{:08X}",
                   isCall ? "call" : "jump", site, target, entry);
}

//=============================================================================
// Resolution and Expansion (called during Merge phase)
//=============================================================================

size_t FunctionGraph::tryResolveFunction(uint32_t entry) {
  auto* node = getFunction(entry);
  if (!node || node->isSealed())
    return 0;

  size_t resolved = 0;

  // Get copy of unresolved jumps (list will be modified)
  auto jumps = node->unresolvedJumps();

  REXCODEGEN_TRACE("FunctionGraph::tryResolveFunction 0x{:08X}: {} unresolved jumps", entry,
                   jumps.size());

  for (const auto& jump : jumps) {
    // Try internal label first
    if (node->tryResolveAsInternalLabel(jump.target)) {
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as internal label", jump.site, jump.target);
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    // Try function entry
    if (auto* targetFn = getFunction(jump.target)) {
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as {} to function {}", jump.site,
                       jump.target, jump.isCall ? "call" : "tail call", targetFn->name());
      if (jump.isCall) {
        node->addCall(jump.site, CallTarget::function(targetFn));
      } else {
        node->addTailCall(jump.site, CallTarget::function(targetFn));
      }
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    // Try import
    if (isImport(jump.target)) {
      auto* importNode = getFunction(jump.target);
      const std::string& importName = importNode->name();
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as {} to import {}", jump.site, jump.target,
                       jump.isCall ? "call" : "tail call", importName);
      if (jump.isCall) {
        node->addCall(jump.site, CallTarget::import(jump.target, importName));
      } else {
        node->addTailCall(jump.site, CallTarget::import(jump.target, importName));
      }
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: still unresolved", jump.site, jump.target);
  }

  return resolved;
}

void FunctionGraph::absorbRegionIntoFunction(uint32_t entry, uint32_t regionBase,
                                             uint32_t regionSize) {
  if (auto* node = getFunction(entry)) {
    node->absorbRegion(regionBase, regionSize);
  }
}

bool FunctionGraph::trySealFunction(uint32_t entry) {
  auto* node = getFunction(entry);
  if (!node || node->isSealed())
    return false;

  if (node->canSeal()) {
    node->seal();
    return true;
  }
  return false;
}

size_t FunctionGraph::sealAllReady() {
  size_t sealed = 0;
  size_t couldNotSeal = 0;
  for (auto& [base, node] : functions_) {
    if (node->isPending()) {
      if (node->canSeal()) {
        node->seal();
        sealed++;
      } else {
        couldNotSeal++;
        REXCODEGEN_DEBUG("FunctionGraph::sealAllReady: 0x{:08X} ({}) cannot seal ({} unresolved)",
                         base, node->name(), node->unresolvedJumps().size());
        for (const auto& jump : node->unresolvedJumps()) {
          REXCODEGEN_DEBUG("  0x{:08X} -> 0x{:08X}", jump.site, jump.target);
        }
      }
    }
  }
  REXCODEGEN_DEBUG("FunctionGraph::sealAllReady: sealed {} functions, {} could not seal", sealed,
                   couldNotSeal);
  return sealed;
}

void FunctionGraph::sealAll() {
  std::vector<std::pair<uint32_t, std::string>> errors;

  for (auto& [base, node] : functions_) {
    if (node->isSealed())
      continue;

    if (!node->canSeal()) {
      std::string reason;
      if (node->isRegistered()) {
        reason = "still in kRegistered state (blocks not discovered)";
      } else if (node->blocks().empty() && !node->isImport()) {
        reason = "has 0 blocks (non-import)";
      } else if (!node->unresolvedJumps().empty()) {
        reason = fmt::format("{} unresolved jumps", node->unresolvedJumps().size());
      } else {
        reason = "unknown reason";
      }
      errors.emplace_back(base, fmt::format("{} (0x{:08X}): {}", node->name(), base, reason));
      continue;
    }

    node->seal();
  }

  if (!errors.empty()) {
    std::string msg = fmt::format("sealAll: {} functions cannot be sealed:\n", errors.size());
    for (const auto& [addr, err] : errors) {
      msg += fmt::format("  - {}\n", err);
    }
    throw std::runtime_error(msg);
  }

  REXCODEGEN_TRACE("FunctionGraph::sealAll: all {} functions sealed", functions_.size());
}

//=============================================================================
// Vacancy Checking
//=============================================================================

void FunctionGraph::registerChunk(uint32_t base, uint32_t size, uint32_t parent) {
  chunks_.push_back({base, size, parent});
  REXCODEGEN_TRACE("FunctionGraph: registered chunk 0x{:08X}-0x{:08X} parent=0x{:08X}",
                   base, base + size, parent);
}

bool FunctionGraph::isVacant(uint32_t fromAddr, uint32_t targetAddr) const {
  // Rule 1: Check for null dword at boundary
  if (memoryReader_) {
    if (targetAddr > fromAddr) {
      // Check for null at target (typical inter-function padding)
      auto val = memoryReader_(targetAddr);
      if (val && *val == 0x00000000) {
        REXCODEGEN_TRACE("FunctionGraph::isVacant: null dword at 0x{:08X} blocks vacancy",
                         targetAddr);
        return false;
      }
    }
  }

  // Rule 2: Check if any chunk claims the target region
  for (const auto& chunk : chunks_) {
    if (targetAddr >= chunk.base && targetAddr < chunk.base + chunk.size) {
      REXCODEGEN_TRACE("FunctionGraph::isVacant: chunk 0x{:08X}-0x{:08X} claims 0x{:08X}",
                       chunk.base, chunk.base + chunk.size, targetAddr);
      return false;
    }
  }

  // Rule 3: Check if target falls within any function's range
  for (const auto& [base, node] : functions_) {
    if (node->containsAddress(targetAddr)) {
      // Target is within a function - check if it's mergeable
      if (isMergeableEntryPoint(targetAddr)) {
        // GAP_FILL can be absorbed
        REXCODEGEN_TRACE("FunctionGraph::isVacant: 0x{:08X} is mergeable (GAP_FILL)", targetAddr);
        continue;  // Don't block, it's mergeable
      }
      // Protected function - blocks vacancy
      REXCODEGEN_TRACE("FunctionGraph::isVacant: 0x{:08X} is within protected function 0x{:08X}",
                       targetAddr, base);
      return false;
    }
  }

  // green light for vacancy
  return true;
}

size_t FunctionGraph::markFuncletRegisterSharing() {
  // MSVC compiles __finally / __except bodies as funclets that run on the
  // owner's frame with its non-volatiles still live, and reaches an inline one
  // by bl from the middle of the owner. Localizing the funclet's copies hands it
  // a zero. The scope table names each funclet, which is the only reliable way
  // to tell one from a real function: every prologue reads its non-volatiles in
  // order to spill them, so "reads before writes" cannot distinguish them. C++
  // EH cleanup and catch funclets run the same way, so they are claimed too. The
  // owner stays localized and syncs across the call site instead, so it keeps
  // its isolation from its own callers.
  std::vector<std::pair<uint32_t, uint32_t>> ranges;  // funclet [base, end)

  for (auto& entry : functions_) {
    auto& node = entry.second;
    if (!node->hasExceptionInfo())
      continue;

    auto claim = [&](uint32_t addr) {
      auto it = addr ? functions_.find(addr) : functions_.end();
      if (it == functions_.end())
        return;
      ranges.emplace_back(it->second->base(), it->second->end());
    };

    if (const auto* seh = node->exceptionInfo()->asSeh()) {
      for (const auto& scope : seh->scopes) {
        claim(scope.handler);
        claim(scope.filter);
      }
    } else if (const auto* cxx = node->exceptionInfo()->asCxx()) {
      for (const auto& entry : cxx->unwindMap) {
        claim(entry.action);
      }
      for (const auto& tryBlock : cxx->tryBlocks) {
        for (const auto& handler : tryBlock.handlers) {
          claim(handler.handlerAddress);
        }
      }
    }
  }

  if (ranges.empty())
    return 0;

  std::sort(ranges.begin(), ranges.end());
  size_t out = 0;
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first <= ranges[out].second) {
      ranges[out].second = std::max(ranges[out].second, ranges[i].second);
    } else {
      ranges[++out] = ranges[i];
    }
  }
  ranges.resize(out + 1);

  auto insideFunclet = [&](uint32_t addr) {
    auto it = std::upper_bound(
        ranges.begin(), ranges.end(), addr,
        [](uint32_t a, const std::pair<uint32_t, uint32_t>& r) { return a < r.first; });
    return it != ranges.begin() && addr < (--it)->second;
  };

  // A funclet compiled with both an unwind entry and an inline bl entry lands in
  // the graph as two nodes sharing one tail. The scope table names only the
  // first, so sweep the extent for the rest.
  size_t marked = 0;
  for (const auto& [base, node] : functions_) {
    if (insideFunclet(base) && !node->sharesRegisters()) {
      node->setSharesRegisters(true);
      ++marked;
    }
  }

  return marked;
}

bool FunctionGraph::isMergeableEntryPoint(uint32_t addr) const {
  // Only entry points can be mergeable
  auto it = functions_.find(addr);
  if (it == functions_.end()) {
    return false;  // Not an entry point
  }

  const FunctionNode* node = it->second.get();

  // Only GAP_FILL authority can be absorbed
  // All other authorities represent immutable entry points
  return node->authority() == FunctionAuthority::GAP_FILL;
}

TargetKind FunctionGraph::classifyTarget(uint32_t target, const FunctionNode& caller,
                                         bool isCallInstruction) const {
  // Case 1: Target is an import - always a call/tail-call
  if (isImport(target)) {
    return TargetKind::Import;
  }

  // Case 2: Target is the caller's own entry point
  if (target == caller.base()) {
    // bl to own base = recursive call (Function)
    // b to own base = loop back to start (InternalLabel)
    return isCallInstruction ? TargetKind::Function : TargetKind::InternalLabel;
  }

  // Case 3: A branch into a configured chunk owned by this exact function is
  // internal. The chunk remains a callable function for all other callers.
  if (!isCallInstruction) {
    const bool isOwnedChunk = std::ranges::any_of(chunks_, [&](const ChunkInfo& chunk) {
      return chunk.base == target && chunk.parent == caller.base();
    });
    if (isOwnedChunk && caller.containsAddress(target)) {
      return TargetKind::InternalLabel;
    }
  }

  // Case 4: Target is a DIFFERENT function's entry point - this is a call/tail-call
  // This handles cases where a small thunk function branches to another function
  // whose entry point happens to fall within the thunk's address range
  if (isEntryPoint(target)) {
    return TargetKind::Function;
  }

  // Case 5: Target is inside caller's function -> InternalLabel
  // For bl, this would be a rare PIC code pattern
  if (caller.containsAddress(target)) {
    return TargetKind::InternalLabel;
  }

  // Case 6: Unknown target
  return TargetKind::Unknown;
}

void FunctionGraph::notifyFunctionAdded(FunctionNode* newFunction) {
  for (auto& [base, node] : functions_) {
    if (node.get() != newFunction && node->isPending()) {
      node->tryResolveAgainst(newFunction);
    }
  }
}

}  // namespace rex::codegen
