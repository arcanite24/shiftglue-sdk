/**
 * @file        rexcodegen/builders/context.cpp
 * @brief       PPC context instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/codegen/binary_view.h>
#include <rex/logging.h>

#include "../codegen_logging.h"
#include <rex/system/export_resolver.h>

namespace rex::codegen {

/// eieio instruction encoding (big-endian). Used for MMIO detection:
/// if the next instruction after a load/store is eieio, the access is MMIO.
static constexpr uint32_t kEieioEncoding = 0xAC06007C;

//=============================================================================
// Convenience Accessors
//=============================================================================

const RecompilerConfig& BuilderContext::config() const {
  return emitCtx.config;
}

const FunctionGraph& BuilderContext::graph() const {
  return emitCtx.graph;
}

//=============================================================================
// Register Accessors
//=============================================================================

// Localizing a non-volatile assumes the function owns it across its whole body.
// An SEH funclet does not: it inherits its owner's live registers through ctx.
bool BuilderContext::localizeNonVolatiles() const {
  // The save/restore helpers read and write the caller's registers through
  // ctx for the share_registers functions that still call them; a localized
  // caller inlines their bodies on its locals instead.
  // register_locals_functions (NN-5.1) localizes every register, so the
  // non-volatiles too; emitCpp only sets allRegisters for eligible functions.
  if (locals.allRegisters)
    return true;
  return config().nonVolatileRegistersAsLocalVariables && !fn.sharesRegisters() &&
         !isSaveRestoreHelper(fn.name());
}

bool BuilderContext::isReferenceCopy() const {
  return emitCtx.namePrefix == "ref_" ||
         (emitCtx.oracleMembers != nullptr && !emitCtx.regionRealCalls);
}

// Region oracle copies: the callee inside the region's copies, or "" when the
// call leaves the region (the caller then emits REX_ORACLE_OPAQUE).
std::string BuilderContext::oracleCallee(const rex::codegen::FunctionNode& fn) const {
  if (!emitCtx.oracleMembers || fn.authority() == rex::codegen::FunctionAuthority::IMPORT ||
      !emitCtx.oracleMembers->count(static_cast<uint32_t>(fn.base())))
    return {};
  return emitCtx.oraclePrefix + fn.name();
}

bool BuilderContext::isSaveRestoreHelper(std::string_view name) {
  return name.find("__rest") == 0 || name.find("__save") == 0;
}

void BuilderContext::publishLocalNonVolatiles(std::string_view indent) {
  for (size_t i = 14; i < 32; ++i) {
    if (locals.r[i])
      println("{}ctx.r{} = r{};", indent, i, i);
    if (locals.f[i])
      println("{}ctx.f{} = f{};", indent, i, i);
  }
  for (size_t i = 14; i < 128; ++i) {
    if (i >= 32 && i < 64)
      continue;
    if (locals.v[i])
      println("{}ctx.v{} = v{};", indent, i, i);
  }
}

// A localized function keeps the registers a save/restore helper would store
// or load in locals the helper cannot see, so the helper's body is inlined
// with the caller's locals in place of ctx: the frame holds exactly what the
// guest code expects, and the restore leaves the locals at their entry values
// for the publish at return. The helper's own generated body is the source,
// rendered once and cached, so every helper variant is covered.
void BuilderContext::inlineSaveRestoreHelper(const rex::codegen::FunctionNode& helper,
                                             std::string_view indent, bool tail) {
  static std::mutex cacheMutex;
  static std::unordered_map<std::string, std::vector<std::string>> cache;
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    // Reference copies render the helper with their own options.
    const std::string key = emitCtx.namePrefix + helper.name();
    auto it = cache.find(key);
    if (it == cache.end()) {
      rex::codegen::EmitContext sub = emitCtx;
      sub.referenced = nullptr;
      sub.referenceBlocked = nullptr;
      const std::string body = helper.emitCpp(sub);
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
        if (code.starts_with("//") || code.starts_with("DEFINE_REX_FUNC") ||
            code.starts_with("REX_FUNC_PROLOGUE") || code.starts_with("REX_PROBE_CALL") ||
            code.starts_with("REX_PROBE_INDIRECT") || code == "}" || code == "return;" ||
            code.starts_with("PPC") || code.starts_with("uint32_t ea"))
          continue;
        lines.emplace_back(code);
      }
      it = cache.emplace(key, std::move(lines)).first;
    }
    lines = it->second;
  }
  for (const std::string& line : lines) {
    std::string rewritten;
    size_t pos = 0;
    while (pos < line.size()) {
      size_t at = line.find("ctx.", pos);
      if (at == std::string::npos) {
        rewritten.append(line, pos, std::string::npos);
        break;
      }
      rewritten.append(line, pos, at - pos);
      size_t p = at + 4;
      char kind = p < line.size() ? line[p] : 0;
      size_t q = p + 1;
      while (q < line.size() && std::isdigit(static_cast<unsigned char>(line[q])))
        q++;
      if ((kind == 'r' || kind == 'f' || kind == 'v') && q > p + 1 &&
          (q == line.size() || !std::isalnum(static_cast<unsigned char>(line[q])))) {
        size_t index = std::stoul(std::string(line.substr(p + 1, q - p - 1)));
        std::string name;
        // With every register local (NN-5.1), the helper's frame pointer
        // (r12) and any other register it names are the caller's locals too.
        const bool all = locals.allRegisters;
        if (kind == 'r' && (index >= 14 || (all && index < 32)))
          name = r(index);
        else if (kind == 'f' && (index >= 14 || (all && index < 32)))
          name = f(index);
        else if (kind == 'v' && ((index >= 14 && index < 32) || index >= 64 || (all && index < 128)))
          name = v(index);
        if (!name.empty()) {
          rewritten += name;
          pos = q;
          continue;
        }
      }
      rewritten.append("ctx.");
      pos = at + 4;
    }
    if (rewritten.find("ea") != std::string::npos)
      (void)ea();
    if (rewritten.find("temp") != std::string::npos)
      (void)temp();
    // The marker tells the publish pass that a local written here only takes
    // its entry value back (a restore), which ctx still holds.
    if (rewritten.starts_with("ctx.fpscr."))
      println("{}{}", indent, rewritten);  // the flush-mode pass matches these lines exactly
    else
      println("{}{}  // nv-helper", indent, rewritten);
  }
  if (tail)
    println("{}return;", indent);
}

// host_local_register_saves (Pinyon Shift NEAR_NATIVE_RESEARCH.md NN-1.2).
// ctx is write-through: every callee leaves r14-r31 in ctx as it found them,
// so copying rN-r31 to host locals where the prologue would call __savegprlr_N
// and copying them back where an epilogue reaches __restgprlr_N hands the
// caller the same registers as the helpers' stack round trip. The helper's
// LR slot store (r12 at r1-8) and its reload into r12 and LR stay, so the
// back chain, the LR slot and ctx.lr look as before; only the GPR slots are
// no longer written. The scan in function_graph.cpp sets hostSaveFirst only
// for functions that never touch those slots otherwise. With host_local_lr
// (NN-1.3) the LR slot store and reload become copies through the
// `rex_saved_lr` local as well (ScanHostLocalLr cleared the slot), so r12 and
// ctx.lr still take the saved value and only the guest stack word is gone.
bool BuilderContext::emitHostLocalSaveRestore(std::string_view name, std::string_view indent,
                                              bool tail) {
  const int first = locals.hostSaveFirst;
  if (const int n = SaveRestoreHelperFirstRegister(name, "__savegprlr_"); n >= 0) {
    if (n != first)
      return false;
    for (int i = n; i < 32; ++i)
      println("{}rex_save_r{} = {}.u64;", indent, i, r(i));
    // host_local_lr (NN-1.3): the LR slot lives in a local, not at r1-8.
    if (locals.hostLr) {
      println("{}rex_saved_lr = {}.u32;", indent, r(12));
      if (config().abiPoisonChecks)
        println("{}REX_STACK_STORE_U32({}.u32 + -8, REX_LR_SLOT_POISON(0x{:08X}));", indent, r(1),
                fn.base());
    } else
      println("{}REX_STACK_STORE_U32({}.u32 + -8, {}.u32);", indent, r(1), r(12));
    if (tail)
      println("{}return;", indent);
    return true;
  }
  if (const int n = SaveRestoreHelperFirstRegister(name, "__restgprlr_"); n >= 0) {
    if (n < first)
      return false;
    for (int i = n; i < 32; ++i)
      println("{}{}.u64 = rex_save_r{};", indent, r(i), i);
    if (locals.hostLr) {
      if (config().abiPoisonChecks)
        println("{}REX_LR_SLOT_CHECK(0x{:08X}, 0x{:08X}, REX_STACK_LOAD_U32({}.u32 + -8));", indent,
                fn.base(), base, r(1));
      println("{}{}.u64 = rex_saved_lr;", indent, r(12));
    } else
      println("{}{}.u64 = REX_STACK_LOAD_U32({}.u32 + -8);", indent, r(12), r(1));
    if (!config().skipLr)
      println("{}ctx.lr = {}.u64;", indent, r(12));
    if (tail)
      println("{}return;", indent);
    return true;
  }
  return false;
}

void BuilderContext::reloadLocalNonVolatiles(std::string_view indent) {
  for (size_t i = 14; i < 32; ++i) {
    if (locals.r[i])
      println("{}r{} = ctx.r{};", indent, i, i);
    if (locals.f[i])
      println("{}f{} = ctx.f{};", indent, i, i);
  }
  for (size_t i = 14; i < 128; ++i) {
    if (i >= 32 && i < 64)
      continue;
    if (locals.v[i])
      println("{}v{} = ctx.v{};", indent, i, i);
  }
}

std::string BuilderContext::r(size_t index) {
  const auto& cfg = config();
  if (locals.allRegisters ||
      (cfg.nonArgumentRegistersAsLocalVariables &&
       (index == 0 || index == 2 || index == 11 || index == 12)) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.r[index] = true;
    return fmt::format("r{}", index);
  }
  return fmt::format("ctx.r{}", index);
}

std::string BuilderContext::f(size_t index) {
  const auto& cfg = config();
  if (locals.allRegisters || (cfg.nonArgumentRegistersAsLocalVariables && index == 0) ||
      (localizeNonVolatiles() && cfg.nonVolatileFprsAsLocalVariables && index >= 14)) {
    locals.f[index] = true;
    return fmt::format("f{}", index);
  }
  return fmt::format("ctx.f{}", index);
}

std::string BuilderContext::v(size_t index) {
  const auto& cfg = config();
  if (locals.allRegisters ||
      (cfg.nonArgumentRegistersAsLocalVariables && (index >= 32 && index <= 63)) ||
      (localizeNonVolatiles() && cfg.nonVolatileVectorsAsLocalVariables &&
       ((index >= 14 && index <= 31) || (index >= 64 && index <= 127)))) {
    locals.v[index] = true;
    return fmt::format("v{}", index);
  }
  return fmt::format("ctx.v{}", index);
}

std::string BuilderContext::cr(size_t index) {
  if (config().crRegistersAsLocalVariables) {
    locals.cr[index] = true;
    return fmt::format("cr{}", index);
  }
  return fmt::format("ctx.cr{}", index);
}

const char* BuilderContext::ctr() {
  if (config().ctrAsLocalVariable) {
    locals.ctr = true;
    return "ctr";
  }
  return "ctx.ctr";
}

const char* BuilderContext::xer() {
  if (config().xerAsLocalVariable) {
    locals.xer = true;
    return "xer";
  }
  return "ctx.xer";
}

const char* BuilderContext::reserved() {
  if (config().reservedRegisterAsLocalVariable) {
    locals.reserved = true;
    return "reserved";
  }
  return "ctx.reserved";
}

const char* BuilderContext::temp() {
  locals.temp = true;
  return "temp";
}

const char* BuilderContext::v_temp() {
  locals.v_temp = true;
  return "vTemp";
}

const char* BuilderContext::env() {
  locals.env = true;
  return "env";
}

const char* BuilderContext::ea() {
  locals.ea = true;
  return "ea";
}

//=============================================================================
// Output Helpers
//=============================================================================

// Template implementations in header, but we need explicit instantiations
// for common format strings to avoid link errors in some cases

//=============================================================================
// Code Generation Helpers
//=============================================================================

bool BuilderContext::mmio_check_d_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[2]);
}

bool BuilderContext::mmio_check_x_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[1]) || locals.is_mmio_base(insn.operands[2]);
}

const CallTarget* BuilderContext::findCallTarget(uint32_t site) const {
  // Search in calls (bl instructions)
  for (const auto& edge : fn.calls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  // Search in tail calls (b instructions to other functions)
  for (const auto& edge : fn.tailCalls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  return nullptr;
}

std::string BuilderContext::calleeSymbol(const rex::codegen::FunctionNode& fn) const {
  const auto& cfg = config();
  // Only generated guest functions have the weak thunk over a strong `__imp__`
  // body; a [rexcrt] hook or a save/restore helper is named directly.
  if (!cfg.directCallsBypassThunk || fn.authority() == rex::codegen::FunctionAuthority::IMPORT ||
      !fn.name().starts_with("sub_"))
    return fn.name();
  if (std::binary_search(cfg.hostOverriddenFunctions.begin(), cfg.hostOverriddenFunctions.end(),
                         static_cast<uint32_t>(fn.base())))
    return fn.name();
  return "__imp__" + fn.name();
}

void BuilderContext::emit_function_call(uint32_t address) {
  const auto& cfg = config();
  // A native region copy calls region functions' copies but makes every
  // other call for real.
  const bool oracle = emitCtx.oracleMembers != nullptr && !emitCtx.regionRealCalls;
  const bool region = emitCtx.oracleMembers != nullptr;
  auto opaque = [&](std::string_view indent) {
    println("{}REX_ORACLE_OPAQUE(ctx, base, 0x{:08X}u, 0x{:08X}u);", indent, address, base);
  };
  if (oracle && (address == cfg.longJmpAddress || address == cfg.setJmpAddress)) {
    opaque("\t");
    return;
  }

  if (address == cfg.longJmpAddress) {
    // Use custom ppc_longjmp that uses guest address as key (not for storage)
    println("\tppc_longjmp({}.u32, {}.s32);", r(3), r(4));
    return;
  }

  if (address == cfg.setJmpAddress) {
    // Save PPCContext for restoration after longjmp
    println("\t{} = ctx;", env());
    // Use custom ppc_setjmp that uses guest address as key
    println("\t{}.s64 = ppc_setjmp({}.u32);", temp(), r(3));
    // Restore PPCContext if returning from longjmp
    println("\tif ({}.s64 != 0) ctx = {};", temp(), env());
    println("\t{} = {};", r(3), temp());
    return;
  }

  // Try to use pre-resolved call target from FunctionGraph
  if (const auto* target = findCallTarget(base)) {
    if (target->isFunction()) {
      auto* targetFn = target->asFunction();
      const auto& name = targetFn->name();
      // The strong `__imp__` definition when the host does not override the
      // function (cfg.directCallsBypassThunk), so Clang can inline it.
      const std::string callee = calleeSymbol(*targetFn);

      // A localized function keeps its non-volatiles in locals the helper
      // cannot see, so the helper's body is inlined on them. A share_registers
      // function works in ctx and keeps the call (the helper bodies are never
      // localized).
      // host_local_register_saves: the save-area scan cleared this function
      // to keep its saved registers in host locals instead of the stack slots.
      if (locals.hostSaveFirst && emitHostLocalSaveRestore(name, "\t", false))
        return;

      // A reference copy (NN-5.1) inlines the helpers as they are, so their
      // stack stores go through the logged macros and the copy can be rolled
      // back; a call would write the frame behind the log's back.
      if ((localizeNonVolatiles() || isReferenceCopy() || emitCtx.oracleMembers) &&
          isSaveRestoreHelper(name)) {
        inlineSaveRestoreHelper(*targetFn, "\t", false);
        return;
      }

      // A region oracle copy stays inside the region's copies.
      std::string inside;
      if (region) {
        inside = oracleCallee(*targetFn);
        if (inside.empty() && oracle) {
          opaque("\t");
          return;
        }
        if (!inside.empty())
          emitCtx.reference(inside);
      }
      const std::string& target_symbol = inside.empty() ? callee : inside;

      // An SEH funclet runs on its owner's frame and reads whatever non-volatiles
      // the owner left live, and a share_registers function reads the caller's
      // registers through ctx, so hand them the localized copies and take them
      // back afterwards. Only registers already localized here can be live at
      // this point, so that set is the whole live-in the callee can see.
      if (targetFn->sharesRegisters() && localizeNonVolatiles()) {
        publishLocalNonVolatiles("\t");
        emitCtx.reference(name);
        println("\t{}(ctx, base);", target_symbol);
        reloadLocalNonVolatiles("\t");
        return;
      }

      // The save/restore helpers take the frame pointer in r12 and are not
      // ABI calls, so the checks skip them.
      const bool poison = cfg.abiPoisonChecks &&
                          !(name.find("__rest") == 0 || name.find("__save") == 0);
      emitCtx.reference(name);
      if (poison)
        println("\tREX_ABI_POISON_PRE(0x{:08X});", base);
      println("\t{}(ctx, base);", target_symbol);
      if (poison)
        println("\tREX_ABI_POISON_POST(0x{:08X});", base);
      return;
    }

    if (target->isImport()) {
      if (oracle) {
        opaque("\t");
        return;
      }
      const auto& importTarget = std::get<CallTarget::ToImport>(target->value);
      std::string func_name;

      // Try to resolve ordinal to actual function name
      auto at_pos = importTarget.name.find('@');
      if (at_pos != std::string::npos && emitCtx.resolver) {
        auto lib_name = importTarget.name.substr(0, at_pos);
        auto ordinal_str = importTarget.name.substr(at_pos + 1);
        uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));

        auto* exp = emitCtx.resolver->GetExportByOrdinal(lib_name + ".xex", ordinal);
        if (!exp)
          exp = emitCtx.resolver->GetExportByOrdinal(lib_name, ordinal);

        if (exp) {
          func_name = "__imp__" + std::string(exp->name);
        }
      }

      if (func_name.empty()) {
        func_name = "__imp__" + importTarget.name;
        std::replace(func_name.begin(), func_name.end(), '@', '_');
        std::replace(func_name.begin(), func_name.end(), '.', '_');
      }

      emitCtx.reference(func_name);
      if (cfg.abiPoisonChecks)
        println("\tREX_ABI_POISON_PRE(0x{:08X});", base);
      println("\t{}(ctx, base);", func_name);
      if (cfg.abiPoisonChecks)
        println("\tREX_ABI_POISON_POST(0x{:08X});", base);
      return;
    }

    // Unresolved target from graph
    REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X}", address, base);
    println("\t// FATAL: unresolved function 0x{:08X}", address);
    println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
    return;
  }

  // No pre-resolved target found - this is an error
  REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X} (no CallTarget in FunctionNode)",
                   address, base);
  println("\t// FATAL: unresolved function 0x{:08X} (no CallTarget in FunctionNode)", address);
  println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
}

void BuilderContext::emit_tail_function_call(uint32_t address, std::string_view indent) {
  const auto& cfg = config();
  // A native region copy calls region functions' copies but makes every
  // other call for real.
  const bool oracle = emitCtx.oracleMembers != nullptr && !emitCtx.regionRealCalls;
  const bool region = emitCtx.oracleMembers != nullptr;
  auto opaque = [&] {
    println("{}REX_ORACLE_OPAQUE(ctx, base, 0x{:08X}u, 0x{:08X}u);", indent, address, base);
  };
  if (oracle && (address == cfg.longJmpAddress || address == cfg.setJmpAddress)) {
    opaque();
    return;
  }

  if (address == cfg.longJmpAddress || address == cfg.setJmpAddress) {
    emit_function_call(address);
    println("{}return;", indent);
    return;
  }

  if (const auto* target = findCallTarget(base)) {
    if (target->isFunction()) {
      auto* targetFn = target->asFunction();
      const auto& name = targetFn->name();
      // The strong `__imp__` definition when the host does not override the
      // function (cfg.directCallsBypassThunk), so Clang can inline it.
      const std::string callee = calleeSymbol(*targetFn);
      if (locals.hostSaveFirst && emitHostLocalSaveRestore(name, indent, true))
        return;
      if ((localizeNonVolatiles() || isReferenceCopy() || emitCtx.oracleMembers) &&
          isSaveRestoreHelper(name)) {
        inlineSaveRestoreHelper(*targetFn, indent, true);
        return;
      }
      // A tail branch never comes back to this frame. Publish localized
      // non-volatiles before entering a shared-register function, but do not
      // emit the normal post-call reload sequence.
      if (targetFn->sharesRegisters() && localizeNonVolatiles()) {
        publishLocalNonVolatiles(indent);
      }
      if (region) {
        const std::string inside = oracleCallee(*targetFn);
        if (inside.empty() && oracle) {
          opaque();
          return;
        }
        if (!inside.empty()) {
          emitCtx.reference(inside);
          println("{}REX_TAIL_CALL({});", indent, inside);
          return;
        }
      }
      emitCtx.reference(name);
      println("{}REX_TAIL_CALL({});", indent, callee);
      return;
    }

    if (target->isImport()) {
      if (oracle) {
        opaque();
        return;
      }
      const auto& importTarget = std::get<CallTarget::ToImport>(target->value);
      std::string func_name;
      auto at_pos = importTarget.name.find('@');
      if (at_pos != std::string::npos && emitCtx.resolver) {
        auto lib_name = importTarget.name.substr(0, at_pos);
        auto ordinal_str = importTarget.name.substr(at_pos + 1);
        uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));
        auto* exp = emitCtx.resolver->GetExportByOrdinal(lib_name + ".xex", ordinal);
        if (!exp)
          exp = emitCtx.resolver->GetExportByOrdinal(lib_name, ordinal);
        if (exp)
          func_name = "__imp__" + std::string(exp->name);
      }
      if (func_name.empty()) {
        func_name = "__imp__" + importTarget.name;
        std::replace(func_name.begin(), func_name.end(), '@', '_');
        std::replace(func_name.begin(), func_name.end(), '.', '_');
      }
      emitCtx.reference(func_name);
      println("{}REX_TAIL_CALL({});", indent, func_name);
      return;
    }
  }

  REXCODEGEN_ERROR("Unresolved tail call 0x{:08X} from 0x{:08X}", address, base);
  println("{}REX_FATAL(\"Unresolved tail call from 0x{:08X} to 0x{:08X}\");", indent, base,
          address);
}

void BuilderContext::emit_conditional_branch(bool not_, std::string_view cond) {
  uint32_t target = insn.operands[1];

  // Use classifyTarget for consistent branch classification
  // false = branch instruction (not a call), so own-base means loop back
  auto kind = graph().classifyTarget(target, fn, false);

  switch (kind) {
    case TargetKind::InternalLabel:
      // Target is within this function - local goto
      println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond,
              target);
      break;

    case TargetKind::Function:
    case TargetKind::Import:
      // Conditional guest branches are tail branches too.
      if (findCallTarget(base)) {
        println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
        emit_tail_function_call(target, "\t\t");
        println("\t}}");
      } else {
        REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X} (no CallTarget)",
                         target, base);
        println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
                not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      }
      break;

    case TargetKind::Unknown:
      REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X}", target, base);
      println("\t// ERROR: conditional branch to unknown address 0x{:08X}", target);
      println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
              not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      break;
  }
}

void BuilderContext::emit_set_flush_mode(bool enable) {
  auto newState = enable ? CSRState::VMX : CSRState::FPU;
  if (csrState != newState) {
    auto prefix = enable ? "enable" : "disable";
    auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
    println("\tctx.fpscr.{}FlushMode{}();", prefix, suffix);

    csrState = newState;
  }
}

bool BuilderContext::has_mid_asm_hook() const {
  return config().midAsmHooks.find(base) != config().midAsmHooks.end();
}

void BuilderContext::emit_mid_asm_hook() {
  auto midAsmHook = config().midAsmHooks.find(base);
  if (midAsmHook == config().midAsmHooks.end()) {
    return;
  }

  bool returnsBool = midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
                     midAsmHook->second.jumpAddressOnFalse != 0 ||
                     midAsmHook->second.jumpAddressOnTrue != 0;

  print("\t");
  if (returnsBool)
    print("if (");

  // Build call -- no ctx/base prefix, just register arguments resolved through accessors
  print("{}(", midAsmHook->second.name);
  if (midAsmHook->second.context) {
    out += "ctx, base";
  }
  for (auto& reg : midAsmHook->second.registers) {
    if (out.back() != '(')
      out += ", ";

    switch (reg[0]) {
      case 'c':
        if (reg == "ctr")
          out += ctr();
        else
          out += cr(std::atoi(reg.c_str() + 2));
        break;
      case 'x':
        out += xer();
        break;
      case 'l':
        if (reg == "lr")
          out += "ctx.lr";
        break;
      case 'r':
        if (reg == "reserved")
          out += reserved();
        else
          out += r(std::atoi(reg.c_str() + 1));
        break;
      case 'f':
        if (reg == "fpscr")
          out += "ctx.fpscr";
        else
          out += f(std::atoi(reg.c_str() + 1));
        break;
      case 'v':
        out += v(std::atoi(reg.c_str() + 1));
        break;
    }
  }

  if (returnsBool) {
    println(")) {{");

    if (midAsmHook->second.returnOnTrue)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnTrue != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnTrue);

    println("\t}}");

    println("\telse {{");

    if (midAsmHook->second.returnOnFalse)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnFalse != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnFalse);

    println("\t}}");
  } else {
    println(");");

    if (midAsmHook->second.ret)
      println("\treturn;");
    else if (midAsmHook->second.jumpAddress != 0)
      println("\tgoto loc_{:X};", midAsmHook->second.jumpAddress);
  }
}

//=============================================================================
// Vector (SIMD) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_vec_fp_binary(const char* simd_op) {
  println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_{}_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      v(insn.operands[0]), simd_op, v(insn.operands[1]), v(insn.operands[2]));
}

void BuilderContext::emit_vec_fp_unary_expr(std::string_view simd_expr) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);

  // Replace {vA} placeholder in expression with actual register
  std::string expr(simd_expr);
  size_t pos;
  while ((pos = expr.find("{vA}")) != std::string::npos) {
    expr.replace(pos, 4, vA);
  }

  println("\tsimde_mm_store_ps({}.f32, {});", vD, expr);
}

void BuilderContext::emit_vec_int_binary(const char* simd_op, const char* element_type) {
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[1]), element_type,
      v(insn.operands[2]), element_type);
}

void BuilderContext::emit_vec_int_binary_swapped(const char* simd_op, const char* element_type) {
  // Swapped: op(vB, vA) instead of op(vA, vB) - useful for andnot which has reversed operand
  // semantics
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[2]), element_type,
      v(insn.operands[1]), element_type);
}

void BuilderContext::emit_vec_var_shift(const char* shift_dir, const char* element_type,
                                        uint32_t mask_value) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);
  auto vB = v(insn.operands[2]);
  println("\t{{");
  println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_{}(0x{:X}));", element_type,
          mask_value);
  println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_{}_{}"
      "(a, shift));",
      vD, shift_dir, element_type);
  println("\t}}");
}

//=============================================================================
// Memory (Load/Store) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_load_d_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // D-form: rD = LOAD(rA + D) where operands[0]=rD, operands[1]=D, operands[2]=rA
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  } else if (insn.operands[2] == 1 && strncmp(load_macro, "REX_LOAD_", 9) == 0) {
    // r1 is the stack pointer: guest stacks are never in the physical view.
    snprintf(mm_macro, sizeof(mm_macro), "REX_STACK_LOAD_%s", load_macro + 9);
    macro = mm_macro;
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{});", static_cast<int32_t>(insn.operands[1]));
}

void BuilderContext::emit_load_x_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // X-form: rD = LOAD(rA + rB) where operands[0]=rD, operands[1]=rA, operands[2]=rB
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32);", r(insn.operands[2]));
}

void BuilderContext::emit_store_d_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // D-form: STORE(rA + D, rS) where operands[0]=rS, operands[1]=D, operands[2]=rA
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  } else if (insn.operands[2] == 1 && strncmp(store_macro, "REX_STORE_", 10) == 0) {
    // r1 is the stack pointer: guest stacks are never in the physical view.
    snprintf(mm_macro, sizeof(mm_macro), "REX_STACK_STORE_%s", store_macro + 10);
    macro = mm_macro;
  }

  print("\t{}(", macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{}, {}.{});", static_cast<int32_t>(insn.operands[1]), r(insn.operands[0]), src_type);
}

void BuilderContext::emit_store_x_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // X-form: STORE(rA + rB, rS) where operands[0]=rS, operands[1]=rA, operands[2]=rB
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {  // Use X-form specific check (operands[1] is base)
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  print("\t{}(", macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32, {}.{});", r(insn.operands[2]), r(insn.operands[0]), src_type);
}

}  // namespace rex::codegen
