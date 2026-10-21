/**
 * @file        rexcodegen/internal/config.h
 * @brief       Recompiler configuration types
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/codegen/function_graph.h>  // For JumpTable

namespace rex::codegen {

struct MidAsmHook {
  std::string name;
  std::vector<std::string> registers;

  bool ret = false;
  bool returnOnTrue = false;
  bool returnOnFalse = false;

  uint32_t jumpAddress = 0;
  uint32_t jumpAddressOnTrue = 0;
  uint32_t jumpAddressOnFalse = 0;

  bool afterInstruction = false;

  // Pass the live guest execution context to the host hook as the leading
  // arguments (`ctx, base`). A hook that needs to call recompiled guest code,
  // or read registers the hook configuration does not name, requires this.
  bool context = false;
};

// Unified function/chunk configuration
// A "chunk" is simply a function entry with a non-zero parent field
struct FunctionConfig {
  uint32_t size = 0;    // Explicit size in bytes (mutually exclusive with end)
  uint32_t end = 0;     // End address, exclusive (mutually exclusive with size)
  std::string name;     // Custom symbol name (empty = auto-generate sub_XXXXXXXX)
  uint32_t parent = 0;  // Parent function address (0 = standalone, non-zero = chunk)

  // Keep non-volatiles in ctx instead of localizing them. Marks an MSVC SEH
  // funclet, which reads the registers its owner left live and would see a zero
  // from a local. Callers sync their localized copies across the call site.
  bool shareRegisters = false;

  // Get effective size (prefers size over end)
  uint32_t getSize(uint32_t address) const {
    return size ? size : (end > address ? end - address : 0);
  }
  // Returns true if this is a discontinuous chunk belonging to a parent function
  bool isChunk() const { return parent != 0; }
};

// Section info for analysis output
struct SectionInfo {
  std::string name;
  uint64_t address = 0;
  uint64_t size = 0;
  std::string flags;  // "rx", "rw", "r" etc.
};

// Function entry for analysis output
struct FunctionEntry {
  uint64_t address = 0;
  uint64_t size = 0;
  std::string name;  // optional, defaults to "sub_XXXXXXXX"
};

struct RecompilerConfig {
  // === Required user-provided fields ===
  std::string projectName = "rex";  ///< Project name for output files
  std::string filePath;             ///< Path to XEX/ELF file
  std::string outDirectoryPath;     ///< Output directory for generated code
  std::string templateDir;          ///< Optional custom template directory for overrides

  /// Every TOML that fed this config, in load order. LoadFromTable() gets a
  /// parsed table, so a manifest-embedded entry omits the manifest itself.
  std::vector<std::string> loadedFiles;

  // === Code generation options (optional) ===
  bool skipLr = false;
  bool ctrAsLocalVariable = false;
  bool xerAsLocalVariable = false;
  bool reservedRegisterAsLocalVariable = false;
  bool skipMsr = false;
  bool crRegistersAsLocalVariables = false;
  bool nonArgumentRegistersAsLocalVariables = false;
  /// Keep the non-volatile registers a function uses in C++ locals, publishing
  /// the written ones to ctx at every call and return. Correct on FH1, but its
  /// hot threads spent 7-13 % more cycles per frame on x86-64 than with the
  /// registers in ctx (Pinyon Shift NEXT_LEVEL_RESEARCH.md NL-1.4), so off.
  bool nonVolatileRegistersAsLocalVariables = false;
  /// With non_volatile_as_local: also the FPRs f14-f31 and the VRs v14-v31,
  /// v64-v127 (both on by default; off keeps that class in ctx).
  bool nonVolatileFprsAsLocalVariables = true;
  bool nonVolatileVectorsAsLocalVariables = true;
  /// With non_volatile_as_local: read the locals back from ctx after every
  /// call (a callee that leaves a changed value in one is then seen).
  bool nonVolatileReloadAfterCall = false;
  bool generateExceptionHandlers = false;  ///< Generate SEH exception handler wrappers
  /// Emit a resume switch at every function entry and register every linked-call
  /// return address, so the runtime can re-enter a function at a return address
  /// after a guest stack switch (KeSetCurrentStackPointers). Off when the
  /// title's fiber functions run on host fibers ([rexcrt] CreateFiber,
  /// SwitchToFiber, ...), which keeps the host call chain intact: every
  /// function then has a single entry.
  bool interiorResume = true;
  /// Call a known guest function through its strong `__imp__` definition
  /// instead of the weak `sub_` thunk, so Clang can inline it. Functions the
  /// host overrides with a strong `sub_` of its own must be listed in
  /// hostOverriddenFunctions, or direct calls would skip the override.
  bool directCallsBypassThunk = false;
  /// Replace a load whose address is a constant formed by lis/addi and points
  /// into a read-only section (.rdata) with the literal value. The folded
  /// ranges are hashed and checked against the loaded image at start.
  bool foldRodataLoads = false;
  /// Replace a function's prologue `bl __savegprlr_N` and its `__restgprlr_N`
  /// epilogues with copies of rN-r31 in host locals: ctx is write-through, so
  /// the copy at entry and the copy back at each restore give the caller the
  /// same registers without the guest stack slots. The LR slot store and its
  /// reload stay. Only functions the save-area scan proves never touch the
  /// slots otherwise (no slot loads or stores, no dynamic frame, no tail
  /// branch, no exception info, no hook) are changed; the rest keep the calls.
  bool hostLocalRegisterSaves = false;
  /// Functions that keep the helper calls with host_local_register_saves on
  /// (a host or hook reader of their save slots).
  std::vector<uint32_t> hostLocalRegisterSavesKeep;
  /// With hostLocalRegisterSaves: keep the LR save slot (entry r1 - 8) in a
  /// host local, `rex_saved_lr`, instead of guest stack memory. The prologue's
  /// slot store (the `__savegprlr_N` helper's, or an explicit `stw rS,-8(r1)`)
  /// becomes a copy into the local and every reload of the slot (the
  /// `__restgprlr_N` epilogue's, or `lwz rD,-8(r1)`) a copy back, so r12 and
  /// ctx.lr take exactly the values they took from memory. Only functions
  /// whose LR-slot scan proves that the store and those reloads are the only
  /// accesses to the slot are changed: no other access, no pointer to it, no
  /// dynamic frame, no exception info, no hook, no tail branch to code that
  /// may read the slot before storing it. The back chain is left alone.
  bool hostLocalLr = false;
  /// Functions that keep the LR slot in guest memory with host_local_lr on
  /// (a host or hook reader of the slot, such as a stack walker diagnostic).
  std::vector<uint32_t> hostLocalLrKeep;
  /// Per-function specialisation (Pinyon Shift NEAR_NATIVE_RESEARCH.md NN-5.1),
  /// sorted function entry addresses. register_locals_functions: every GPR,
  /// FPR and VR the function names lives in a C++ local inside it. ctx is
  /// written back before every call, tail call and return (the registers the
  /// function wrote) and the volatile locals are read back after every call,
  /// so ctx holds at each boundary exactly what the translation with the
  /// registers in ctx leaves there, callee-left volatiles included. Functions
  /// with a mid-asm hook, exception info, setjmp/longjmp, a resume switch or
  /// shared registers keep the registers in ctx.
  std::vector<uint32_t> registerLocalsFunctions;
  /// fixed_fp_mode_functions: the flush-mode toggles read the fixed IEEE FP
  /// mode switch (rex::ppc::g_fixed_fp_mode) once at entry instead of at every
  /// site; with fixed mode on, the function clears a leftover flush bit once
  /// and skips every toggle.
  std::vector<uint32_t> fixedFpModeFunctions;
  /// single_precision_functions: fmadds, fmsubs, fnmadds and fnmsubs whose
  /// three operands are known single-precision values (from lfs or another
  /// single-precision result along the straight-line path) are computed as one
  /// single-precision fused multiply-add, as the guest does, instead of a
  /// double fma rounded to single. Results can differ from the double form by
  /// its double-rounding error (one single-precision ulp, rarely).
  std::vector<uint32_t> singlePrecisionFunctions;
  bool isRegisterLocalsFunction(uint32_t address) const {
    return std::binary_search(registerLocalsFunctions.begin(), registerLocalsFunctions.end(),
                              address);
  }
  bool isFixedFpModeFunction(uint32_t address) const {
    return std::binary_search(fixedFpModeFunctions.begin(), fixedFpModeFunctions.end(), address);
  }
  bool isSinglePrecisionFunction(uint32_t address) const {
    return std::binary_search(singlePrecisionFunctions.begin(), singlePrecisionFunctions.end(),
                              address);
  }
  /// Emit `#line <guest offset> "<project>.guest"` before each instruction's
  /// code, so debug line tables (and sample profiles built from them) name
  /// guest addresses and survive changes to the generated C++.
  bool guestLineDirectives = false;
  /// Emit the `// mnemonic operands` comment before each instruction's code.
  bool disassemblyComments = true;
  /// Probe build: count every function entry and record every indirect call's
  /// target (rex/ppc/probe.h); the runtime writes probe.json at shutdown.
  bool callProbes = false;
  /// Functions also emitted a second time as `ref_sub_X` with the reference
  /// options (every register in ctx, no folding, calls through the thunk), so
  /// a host verifier can run both on cloned state and compare (lockstep verify).
  std::vector<uint32_t> referenceFunctions;
  /// Also take every Nth leaf function (no calls) of at least four instructions
  /// as a reference function, 0 for none: a broad sample without a hand list.
  uint32_t referenceSampleEvery = 0;
  /// Region oracle roots: each listed function and every function it reaches
  /// through direct calls is emitted twice more, as `orr_sub_X` (reference
  /// options) and `orc_sub_X` (this build's options), with logged stores and
  /// calls kept inside the region (rex/ppc/oracle.h). A host oracle defines
  /// the strong `sub_X` of each root.
  std::vector<uint32_t> oracleRegions;
  /// Native regions (a subset of oracleRegions): the region's functions are
  /// also emitted as always-inline copies that share one local context, so
  /// calls inside the region pass registers as SSA values instead of
  /// publishing them to PPCContext. The generated `nr_sub_X` runs the
  /// region; the host decides when (rex/ppc/oracle.h).
  std::vector<uint32_t> nativeRegions;
  std::vector<uint32_t> hostOverriddenFunctions;
  /// Test builds: wrap every direct and indirect guest call in REX_ABI_POISON_PRE
  /// and REX_ABI_POISON_POST, which check that the callee preserved the PowerPC
  /// non-volatile registers and poison the volatile ones dead at a return.
  bool abiPoisonChecks = false;

  // === Analysis tuning (optional) ===
  uint32_t maxJumpExtension = 65536;  ///< Max bytes to extend function for jump table targets
  uint32_t dataRegionThreshold = 16;  ///< Consecutive invalid instructions to mark as data region
  uint32_t largeFunctionThreshold = 1048576;  ///< 1MB - warn if function exceeds this size

  // Optional override for DLL module flag. If unset, the orchestrator infers
  // from the module's position in the manifest (entrypoint = false, modules = true).
  std::optional<bool> isDll;

  // === Manual overrides ===
  std::unordered_map<uint32_t, FunctionConfig> functions;  ///< Function/chunk configuration
  std::unordered_map<uint32_t, JumpTable> switchTables;
  std::unordered_map<uint32_t, MidAsmHook> midAsmHooks;
  uint32_t longJmpAddress = 0;
  uint32_t setJmpAddress = 0;

  // === rexcrt: CRT function address overrides ===
  // Maps function name -> guest address (e.g. "CreateFileA" -> 0x8248B780)
  // Parsed from [rexcrt] TOML table. Codegen generates rexcrt_<Name> entries.
  std::unordered_map<std::string, uint32_t> rexcrtFunctions;

  // === User hints (merged with analysis results in AnalysisState) ===
  std::unordered_map<uint32_t, uint32_t> invalidInstructionHints;  ///< addr -> size
  std::unordered_set<uint32_t>
      knownIndirectCallHints;  ///< bctr addresses that are vtable/computed calls
  std::vector<uint32_t> exceptionHandlerFuncHints;  ///< Additional exception handler addresses

  /**
   * Load configuration from a TOML file.
   *
   * Supports an optional `includes` array for layered config. Paths in
   * `includes` resolve relative to the including file's directory.  Merge
   * semantics: scalars last-wins, keyed tables additive (same key = last
   * wins), arrays-of-tables deduplicated by primary key, sets additive.
   *
   * @param configFilePath Path to the TOML config file
   * @return true on success, false on error (parse failure, circular
   *         include, depth exceeded)
   */
  bool Load(const std::string_view& configFilePath);

  /**
   * Load configuration from an in-memory TOML table (e.g. an inline binary
   * entry inside a manifest). Includes referenced from the table resolve
   * relative to `base_dir`. Same merge semantics as Load().
   */
  bool LoadFromTable(const toml::table& tbl, const std::filesystem::path& base_dir);

  /// Validation result containing warnings and errors.
  struct ValidationResult {
    bool valid = true;                  ///< true if no errors (warnings OK)
    std::vector<std::string> warnings;  ///< Non-fatal issues
    std::vector<std::string> errors;    ///< Fatal issues that block codegen

    explicit operator bool() const { return valid; }
  };

  /**
   * Validate the loaded configuration.
   * Checks address alignment, required fields, and sanity constraints.
   * @return ValidationResult with warnings and errors
   */
  ValidationResult Validate() const;
};

}  // namespace rex::codegen
