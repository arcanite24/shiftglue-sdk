/**
 * @file        codegen/codegen_writer.cpp
 * @brief       Consolidated codegen output writer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/codegen_writer.h>
#include "codegen_flags.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <map>
#include <set>
#include <unordered_set>

#include <fmt/format.h>
#include <inja/inja.hpp>

#include <rex/codegen/function_graph.h>
#include <rex/codegen/output_partition.h>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>
#include <rex/ppc/oracle.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>

#include "codegen_logging.h"
#include "file_io.h"
#include "template_registry_internal.h"

namespace {

nlohmann::json buildTemplateData(const rex::codegen::CodegenContext& ctx,
                                 const std::vector<const rex::codegen::FunctionNode*>& functions,
                                 const std::unordered_map<uint32_t, std::string>& rexcrtByAddr) {
  const auto& cfg = ctx.Config();

  // Compute code_base and code_size from binary sections
  size_t codeMin = ~size_t(0);
  size_t codeMax = 0;
  for (const auto& section : ctx.binary().sections()) {
    if (section.executable) {
      if (section.baseAddress < codeMin)
        codeMin = section.baseAddress;
      if ((section.baseAddress + section.size) > codeMax)
        codeMax = section.baseAddress + section.size;
    }
  }

  // Build functions JSON array
  // A discontinuous/overlapping function graph can expose the same linked-call
  // return PC through more than one FunctionNode. Runtime registration replaces
  // earlier mappings with later ones, so choose that same deterministic owner
  // here and emit each resume alias exactly once.
  std::unordered_map<uint32_t, const rex::codegen::FunctionNode*> resumeAliasOwners;
  for (const auto* fn : functions) {
    if (fn->authority() == rex::codegen::FunctionAuthority::IMPORT || !cfg.interiorResume) {
      continue;
    }
    for (uint32_t address : fn->resumableReturnAddresses(ctx.binary())) {
      const auto* explicitEntry = ctx.graph.getFunction(address);
      if (!explicitEntry || explicitEntry == fn) {
        resumeAliasOwners[address] = fn;
      }
    }
  }

  nlohmann::json functionsJson = nlohmann::json::array();
  for (const auto* fn : functions) {
    std::string funcName;
    bool isRexcrt = false;

    auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
    if (crtIt != rexcrtByAddr.end()) {
      funcName = crtIt->second;
      isRexcrt = true;
    } else if (fn->base() == ctx.analysisState().entryPoint) {
      funcName = "xstart";
    } else if (!fn->name().empty()) {
      funcName = fn->name();
    } else {
      funcName = fmt::format("sub_{:08X}", fn->base());
    }

    nlohmann::json resumeAliases = nlohmann::json::array();
    if (fn->authority() != rex::codegen::FunctionAuthority::IMPORT && cfg.interiorResume) {
      for (uint32_t address : fn->resumableReturnAddresses(ctx.binary())) {
        const auto owner = resumeAliasOwners.find(address);
        if (owner != resumeAliasOwners.end() && owner->second == fn) {
          resumeAliases.push_back(fmt::format("0x{:X}", address));
        }
      }
    }

    functionsJson.push_back({
        {"address", fmt::format("0x{:X}", fn->base())},
        {"name", funcName},
        {"is_rexcrt", isRexcrt},
        {"below_code_base", (fn->base() < codeMin)},
        {"is_import", fn->authority() == rex::codegen::FunctionAuthority::IMPORT},
        {"resume_aliases", std::move(resumeAliases)},
    });
  }

  // Build config flags
  nlohmann::json configFlags = {
      {"skip_lr", cfg.skipLr},
      {"ctr_as_local", cfg.ctrAsLocalVariable},
      {"xer_as_local", cfg.xerAsLocalVariable},
      {"reserved_as_local", cfg.reservedRegisterAsLocalVariable},
      {"skip_msr", cfg.skipMsr},
      {"cr_as_local", cfg.crRegistersAsLocalVariables},
      {"non_argument_as_local", cfg.nonArgumentRegistersAsLocalVariables},
      {"non_volatile_as_local", cfg.nonVolatileRegistersAsLocalVariables},
      {"non_volatile_fprs_as_local", cfg.nonVolatileFprsAsLocalVariables},
      {"non_volatile_vectors_as_local", cfg.nonVolatileVectorsAsLocalVariables},
      {"non_volatile_reload_after_call", cfg.nonVolatileReloadAfterCall},
      {"abi_poison_checks", cfg.abiPoisonChecks},
      {"interior_resume", cfg.interiorResume},
      {"register_locals_functions", cfg.registerLocalsFunctions.size()},
      {"fixed_fp_mode_functions", cfg.fixedFpModeFunctions.size()},
      {"single_precision_functions", cfg.singlePrecisionFunctions.size()},
  };

  return {
      {"project", cfg.projectName},
      {"image_base", fmt::format("0x{:X}", ctx.binary().baseAddress())},
      {"image_size", fmt::format("0x{:X}", ctx.binary().imageSize())},
      {"code_base", fmt::format("0x{:X}", codeMin)},
      {"code_size", fmt::format("0x{:X}", codeMax - codeMin)},
      {"rexcrt_heap", cfg.rexcrtFunctions.contains("RtlAllocateHeap") ? 1 : 0},
      {"thunk_reserve_size", fmt::format("0x{:X}", 0x10000u)},
      {"has_dll_modules", ctx.hasDllModules()},
      {"is_dll", ctx.isDllModule()},
      {"config_flags", configFlags},
      {"functions", functionsJson},
      {"recomp_files", nlohmann::json::array()},
  };
}

/// A store statement with `ctx.` dropped and any trailing comment removed, so
/// the reference copy's `REX_STACK_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64)`
/// and a localized body's `REX_STACK_STORE_U64(r1.u32 + -16, r31.u64)` match.
std::string NormalizedStore(std::string_view line) {
  const size_t first = line.find_first_not_of('\t');
  if (first == std::string_view::npos)
    return {};
  std::string_view code = line.substr(first);
  const size_t comment = code.find("  //");
  if (comment != std::string_view::npos)
    code = code.substr(0, comment);
  if (!code.starts_with("REX_STACK_STORE_") && !code.starts_with("REX_STORE_"))
    return {};
  std::string out;
  out.reserve(code.size());
  for (size_t i = 0; i < code.size(); ++i) {
    if (code.compare(i, 4, "ctx.") == 0) {
      i += 3;
      continue;
    }
    out += code[i];
  }
  return out;
}

/// Lockstep verify: the reference copy inlines the save/restore helpers and
/// keeps the link-register slot store, but the optimized function may keep
/// those registers in host locals instead (host_local_register_saves,
/// host_local_lr, register_locals_functions) and never write those frame
/// slots, which nothing else reads (the plans keep the slots in memory
/// wherever something might). Every save-area store of the copy (an inlined
/// helper line, or the LR slot) that the optimized body does not contain is
/// emitted as REX_REF_STORE_UNCHECKED_*: logged and rolled back, never
/// compared. Data stores are not touched, so they are all still compared.
size_t UncheckSaveAreaStores(std::string& reference, const std::string& optimized) {
  std::unordered_set<std::string> present;
  for (size_t pos = 0; pos < optimized.size();) {
    size_t end = optimized.find('\n', pos);
    if (end == std::string::npos)
      end = optimized.size();
    std::string store = NormalizedStore(std::string_view(optimized).substr(pos, end - pos));
    if (!store.empty())
      present.insert(std::move(store));
    pos = end + 1;
  }
  size_t unchecked = 0;
  std::string out;
  out.reserve(reference.size() + 256);
  for (size_t pos = 0; pos < reference.size();) {
    size_t end = reference.find('\n', pos);
    const bool last = end == std::string::npos;
    if (last)
      end = reference.size();
    std::string line = reference.substr(pos, end - pos);
    pos = end + 1;
    const bool saveArea = line.ends_with("// nv-helper") ||
                          line.find("(ctx.r1.u32 + -8, ctx.r12.u32);") != std::string::npos;
    const std::string store = saveArea ? NormalizedStore(line) : std::string();
    if (!store.empty() && !present.count(store)) {
      for (std::string_view macro : {"REX_STACK_STORE_U32(", "REX_STACK_STORE_U64(",
                                     "REX_STORE_U32(", "REX_STORE_U64("}) {
        const size_t at = line.find(macro);
        if (at == std::string::npos)
          continue;
        const bool wide = macro.find("U64") != std::string_view::npos;
        line.replace(at, macro.size(),
                     wide ? "REX_REF_STORE_UNCHECKED_U64(" : "REX_REF_STORE_UNCHECKED_U32(");
        ++unchecked;
        break;
      }
    }
    out += line;
    if (!last)
      out += '\n';
  }
  reference = std::move(out);
  return unchecked;
}

// Native regions (native_regions, rex/ppc/oracle.h): turns the emitted copies
// of a region's functions, named with `prefix`, into always-inline functions
// on the region's local context. Calls between them pass the caller's context
// along; any other call publishes the local context to it, runs there and
// reloads it. `names` receives the functions defined, in order.
bool IsNativeRegionName(std::string_view id, std::string_view prefix) {
  if (id.find("rexcrt_") != std::string_view::npos)
    return false;
  if (id.starts_with("ieee_"))
    id.remove_prefix(5);
  return id.starts_with(prefix);
}

// FPRs in a native region are scalarised into host registers, and a PPCRegister
// read or written through both its double and its integer views ends up an
// integer there, with a move to the FP unit at every arithmetic use (nearly
// 1,000 `fmov` in the arm64 region). The copies touch an FPR as an integer
// only to load, store or fold a constant, so those accesses go through
// std::bit_cast on the double view instead, and the register stays a double.
std::string NativeFloatViews(std::string line) {
  if (line.find("ctx.f") == std::string::npos)
    return line;
  auto fpr = [&](size_t at, size_t& end) -> bool {
    // ctx.f<digits>.<u64|s64> at `at`; `end` past it.
    if (line.compare(at, 5, "ctx.f") != 0 ||
        (at > 0 && (std::isalnum(static_cast<unsigned char>(line[at - 1])) ||
                    line[at - 1] == '_' || line[at - 1] == '.')))
      return false;
    size_t digits = at + 5;
    while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
      ++digits;
    if (digits == at + 5 || line.compare(digits, 4, ".u64") != 0 &&
                                line.compare(digits, 4, ".s64") != 0)
      return false;
    end = digits + 4;
    return end >= line.size() ||
           !(std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_');
  };
  // An assignment to the integer view: the statement's first token.
  size_t lhsEnd = 0;
  const size_t first = line.find_first_not_of(" \t");
  std::string lhs, lhsType;
  if (first != std::string::npos && fpr(first, lhsEnd) &&
      line.compare(lhsEnd, 3, " = ") == 0) {
    lhs = line.substr(first, lhsEnd - first - 4);
    lhsType = line.compare(lhsEnd - 3, 3, "u64") == 0 ? "uint64_t" : "int64_t";
  }
  const size_t rhsStart = lhs.empty() ? 0 : lhsEnd + 3;
  std::string rest;
  for (size_t at = rhsStart; at < line.size();) {
    size_t end;
    if (fpr(at, end)) {
      const std::string reg = line.substr(at, end - at - 4);
      rest += fmt::format("std::bit_cast<{}>({}.f64)",
                          line.compare(end - 3, 3, "u64") == 0 ? "uint64_t" : "int64_t", reg);
      at = end;
    } else {
      rest += line[at++];
    }
  }
  if (lhs.empty())
    return rest;
  // `<rhs>;` possibly followed by a comment.
  const size_t semi = rest.find(';');
  if (semi == std::string::npos)
    return line;
  return fmt::format("{}{}.f64 = std::bit_cast<double>(static_cast<{}>({})){}",
                     line.substr(0, first), lhs, lhsType, rest.substr(0, semi),
                     rest.substr(semi));
}

// The root picks the FP mode once: its fixed-mode body keeps the check that
// enters the IEEE body, the members' fixed-mode bodies drop it, and IEEE
// bodies call the members' IEEE bodies, so each inlined call site carries one
// body instead of both.
std::string NativeRegionTransform(const std::string& text, const std::string& prefix,
                                  const std::string& root, std::vector<std::string>& names) {
  auto identStart = [](const std::string& s, size_t at) {
    while (at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_'))
      --at;
    return at;
  };
  static constexpr std::string_view kDefine = "DEFINE_REX_FUNC(";
  std::unordered_set<std::string> defined;
  for (size_t at = text.find(kDefine); at != std::string::npos;
       at = text.find(kDefine, at + 1)) {
    const size_t close = text.find(')', at);
    defined.insert(text.substr(at + kDefine.size(), close - at - kDefine.size()));
  }
  std::string current;
  std::string out;
  out.reserve(text.size() + text.size() / 8);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos)
      eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;

    if (line.starts_with(kDefine) && line.ends_with(") {")) {
      const std::string name = line.substr(kDefine.size(), line.size() - kDefine.size() - 3);
      if (IsNativeRegionName(name, prefix)) {
        names.push_back(name);
        current = name;
        line = "REX_NR_FN(" + name + ") {";
      }
    }
    const bool ieeeBody = current.starts_with("ieee_");
    if (!ieeeBody && current != prefix + root &&
        line.find("if (!PPCFPSCRRegister::FixedMode())") != std::string::npos &&
        line.find("REX_TAIL_CALL(ieee_") != std::string::npos)
      continue;
    if (ieeeBody) {
      for (size_t at = line.find(prefix); at != std::string::npos;
           at = line.find(prefix, at + 1)) {
        const size_t start = identStart(line, at);
        if (start != at)
          continue;  // already ieee_ or part of a longer name
        size_t end = at;
        while (end < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
          ++end;
        const std::string ieee = "ieee_" + line.substr(at, end - at);
        if (defined.count(ieee)) {
          line.replace(at, end - at, ieee);
          at += 5;
        }
      }
    }

    // Tail calls: into the region, a plain return of the inlined copy; out of
    // it, a real call on the published context (the frame comes back here).
    for (size_t at = line.find("REX_TAIL_CALL("); at != std::string::npos;
         at = line.find("REX_TAIL_CALL(", at + 1)) {
      const size_t open = at + 14;
      const size_t close = line.find(");", open);
      if (close == std::string::npos)
        break;
      const std::string id = line.substr(open, close - open);
      const std::string repl =
          IsNativeRegionName(id, prefix)
              ? "return " + id + "(ctx, base, real);"
              : "{ real = ctx; " + id + "(real, base); ctx = real; return; }";
      line.replace(at, close + 2 - at, repl);
      at += repl.size() - 1;
    }

    // Indirect calls run on the published context: the macro names `ctx`.
    for (size_t at = line.find("REX_CALL_INDIRECT_FUNC("); at != std::string::npos;) {
      const size_t end = line.find(");", at);
      if (end == std::string::npos)
        break;
      const std::string stmt = line.substr(at, end + 2 - at);
      const std::string repl =
          "{ real = ctx; { PPCContext& __restrict ctx = real; " + stmt + " } ctx = real; }";
      line.replace(at, stmt.size(), repl);
      at = line.find("REX_CALL_INDIRECT_FUNC(", at + repl.size());
    }

    // Direct calls.
    static constexpr std::string_view kCall = "(ctx, base);";
    for (size_t at = line.find(kCall); at != std::string::npos;) {
      const size_t start = identStart(line, at);
      const std::string id = line.substr(start, at - start);
      std::string repl;
      if (id.empty() || id == "ctx") {
        at = line.find(kCall, at + kCall.size());
        continue;
      }
      if (IsNativeRegionName(id, prefix))
        repl = id + "(ctx, base, real);";
      else
        repl = "{ real = ctx; " + id + "(real, base); ctx = real; }";
      line.replace(start, at + kCall.size() - start, repl);
      at = line.find(kCall, start + repl.size());
    }
    out += NativeFloatViews(line);
    out += '\n';
  }
  return out;
}

}  // namespace

namespace rex::codegen {

bool IsGeneratedOutputName(std::string_view filename, std::string_view projectName) {
  auto dot = filename.rfind('.');
  if (dot == std::string_view::npos)
    return false;
  auto ext = filename.substr(dot);
  if (ext != ".cpp" && ext != ".h" && ext != ".cmake")
    return false;

  if (filename == "sources.cmake")
    return true;

  constexpr std::array<std::string_view, 4> kPrefixes{"ppc_recomp", "ppc_func_mapping",
                                                      "function_table_init", "ppc_config"};
  for (auto prefix : kPrefixes) {
    if (filename.starts_with(prefix))
      return true;
  }

  std::string projectPrefix(projectName);
  projectPrefix += '_';
  return filename.starts_with(projectPrefix);
}

constexpr size_t kOutputBufferReserveSize = 32 * 1024 * 1024;  // 32 MB

CodegenWriter::CodegenWriter(CodegenContext& ctx, Runtime* runtime)
    : ctx_(ctx), runtime_(runtime) {}

// Convenience accessors
FunctionGraph& CodegenWriter::graph() {
  return ctx_.graph;
}
const FunctionGraph& CodegenWriter::graph() const {
  return ctx_.graph;
}
const BinaryView& CodegenWriter::binary() const {
  return ctx_.binary();
}
RecompilerConfig& CodegenWriter::config() {
  return ctx_.Config();
}
const RecompilerConfig& CodegenWriter::config() const {
  return ctx_.Config();
}
AnalysisState& CodegenWriter::analysisState() {
  return ctx_.analysisState();
}
const AnalysisState& CodegenWriter::analysisState() const {
  return ctx_.analysisState();
}

bool CodegenWriter::write(bool force) {
  deletedFiles_.clear();
  writtenFiles_.clear();
  unchangedFiles_.clear();

  // --- Validation gate (from recompile.cpp) ---
  if (ctx_.errors.HasErrors() && !force) {
    REXCODEGEN_ERROR("Code generation blocked: {} validation errors. Use --force to override.",
                     ctx_.errors.Count());
    return false;
  }

  // --- Output directory setup (from recompile.cpp) ---
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;
  REXCODEGEN_TRACE("Output path: {}", outputPath.string());
  std::filesystem::create_directories(outputPath);

  // --- Everything below from recompiler.cpp recompile() ---
  REXCODEGEN_TRACE("Recompile: starting");
  out.reserve(kOutputBufferReserveSize);

  // Build sorted function list from graph
  std::vector<const FunctionNode*> functions;
  functions.reserve(graph().functionCount());
  for (const auto& [addr, node] : graph().functions()) {
    functions.push_back(node.get());
  }
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->base() < b->base(); });

  // Build rexcrt reverse map and rename graph nodes
  std::unordered_map<uint32_t, std::string> rexcrtByAddr;
  for (const auto& [name, addr] : config().rexcrtFunctions) {
    auto crtName = fmt::format("rexcrt_{}", name);
    rexcrtByAddr[addr] = crtName;
    if (auto* node = graph().getFunction(addr)) {
      node->setName(std::move(crtName));
    }
  }

  const std::string& projectName = config().projectName;

  TemplateRegistry registry;
  if (!config().templateDir.empty())
    registry.loadOverrides(config().templateDir);

  auto tmplData = buildTemplateData(ctx_, functions, rexcrtByAddr);

  // Generate {project}_pch.h (config + macros, stable enough to precompile)
  REXCODEGEN_TRACE("Recompile: generating {}_pch.h", projectName);
  out = renderWithJson(registry, "codegen/pch_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_pch.h", projectName));

  // Generate {project}_funcs.h (every guest function declaration)
  REXCODEGEN_TRACE("Recompile: generating {}_funcs.h", projectName);
  out = renderWithJson(registry, "codegen/funcs_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_funcs.h", projectName));

  // Generate {project}_init.h (the full surface, for init.cpp and consumers)
  REXCODEGEN_TRACE("Recompile: generating {}_init.h", projectName);
  out = renderWithJson(registry, "codegen/init_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.h", projectName));

  // Generate {project}_init.cpp (PPCImageConfig + PPCFuncMappings)
  REXCODEGEN_TRACE("Recompile: generating {}_init.cpp", projectName);
  out = renderWithJson(registry, "codegen/init_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.cpp", projectName));

  // Generate {project}_register.cpp (registration function for hash-based dispatch)
  REXCODEGEN_TRACE("Recompile: generating {}_register.cpp", projectName);
  tmplData["is_dll"] = ctx_.isDllModule();
  out = renderWithJson(registry, "codegen/register_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_register.cpp", projectName));

  // Filter out imports and rexcrt functions before recompilation
  std::erase_if(functions, [](const FunctionNode* fn) {
    return fn->authority() == FunctionAuthority::IMPORT;
  });
  std::erase_if(functions, [&rexcrtByAddr](const FunctionNode* fn) {
    return rexcrtByAddr.contains(static_cast<uint32_t>(fn->base()));
  });

  // Build EmitContext -- resolver is now properly connected
  EmitContext emitCtx{binary(), config(), graph(),
                      static_cast<uint32_t>(analysisState().entryPoint), nullptr};
  if (runtime_)
    emitCtx.resolver = runtime_->export_resolver();

  REXCODEGEN_TRACE("Recompiling {} functions...", functions.size());

  std::vector<std::string> bodies;
  std::vector<FunctionSize> sizes;
  std::vector<std::unordered_set<std::string>> references;
  bodies.reserve(functions.size());
  sizes.reserve(functions.size());
  references.reserve(functions.size());

  const size_t maxFileBytes = REXCVAR_GET(max_file_size_bytes);
  std::unordered_set<std::string> referenced;
  emitCtx.referenced = &referenced;
  std::map<uint32_t, uint32_t> foldedRanges;
  emitCtx.foldedRanges = &foldedRanges;

  // Reference copies (reference_functions, reference_sample_every): the listed
  // functions and every Nth leaf function are emitted a second time as
  // `ref_sub_X` with the reference options and logged stores, for a host
  // verifier that runs both and compares. Their direct callers go through the
  // thunk, so the verifier's strong `sub_X` intercepts every call.
  std::unordered_set<uint32_t> referenceSet(config().referenceFunctions.begin(),
                                            config().referenceFunctions.end());
  if (config().referenceSampleEvery) {
    size_t leaf = 0;
    for (const auto* fn : functions) {
      if (fn->authority() == rex::codegen::FunctionAuthority::IMPORT || !fn->calls().empty() ||
          !fn->tailCalls().empty() || fn->instructions().size() < 4 ||
          !fn->name().starts_with("sub_"))
        continue;
      if (leaf++ % config().referenceSampleEvery == 0)
        referenceSet.insert(static_cast<uint32_t>(fn->base()));
    }
  }
  // Region oracle (oracle_regions, rex/ppc/oracle.h): each root and the
  // functions it reaches through direct calls and tail calls. The save and
  // restore helpers are inlined into the copies, so they get no copy, but
  // their code is part of the region for the host's interpreter.
  struct OracleRegion {
    uint32_t root;
    std::string name;
    std::vector<uint32_t> members;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // [start, end) of code
    std::vector<std::pair<uint32_t, std::string>> crt;   // host CRT functions called
  };
  std::vector<OracleRegion> oracleRegions;
  // Functions the host replaces (the CRT set, host_overridden_functions) have
  // no generated body to copy, and the interpreter must not run their guest
  // code either: a call to one leaves the region, except the CRT functions
  // rex/ppc/oracle.h carries (OracleCrtByName), which the copies call as
  // `orr_rexcrt_<name>` and `orc_rexcrt_<name>`.
  std::unordered_set<uint32_t> hostOnly(config().hostOverriddenFunctions.begin(),
                                        config().hostOverriddenFunctions.end());
  for (const auto& [address, crtName] : rexcrtByAddr)
    hostOnly.insert(address);
  for (uint32_t rootAddress : config().oracleRegions) {
    const auto* rootFn = graph().getFunction(rootAddress);
    if (!rootFn || rootFn->authority() == rex::codegen::FunctionAuthority::IMPORT) {
      REXCODEGEN_ERROR("oracle_regions: no function at 0x{:08X}", rootAddress);
      continue;
    }
    if (referenceSet.count(rootAddress) || hostOnly.count(rootAddress)) {
      REXCODEGEN_ERROR("oracle_regions: 0x{:08X} is also a reference function or replaced by "
                       "the host; the host can define only one strong sub_X", rootAddress);
      continue;
    }
    OracleRegion region{rootAddress, rootFn->name(), {}, {}, {}};
    std::vector<const rex::codegen::FunctionNode*> todo{rootFn};
    std::unordered_set<uint32_t> seen;
    while (!todo.empty()) {
      const auto* fn = todo.back();
      todo.pop_back();
      const uint32_t fnAddress = static_cast<uint32_t>(fn->base());
      if (!seen.insert(fnAddress).second)
        continue;
      if (hostOnly.count(fnAddress)) {
        if (auto crt = rexcrtByAddr.find(fnAddress);
            crt != rexcrtByAddr.end() &&
            rex::ppc::OracleCrtByName(std::string_view(crt->second).substr(7)) !=
                rex::ppc::OracleCrt::kNone)
          region.crt.emplace_back(fnAddress, crt->second);
        continue;
      }
      region.ranges.emplace_back(static_cast<uint32_t>(fn->base()),
                                 static_cast<uint32_t>(fn->end()));
      const std::string& fnName = fn->name();
      if (!(fnName.starts_with("__save") || fnName.starts_with("__rest")))
        region.members.push_back(static_cast<uint32_t>(fn->base()));
      for (const auto* edges : {&fn->calls(), &fn->tailCalls()}) {
        for (const auto& edge : *edges) {
          if (!edge.target.isFunction())
            continue;
          const auto* callee = edge.target.asFunction();
          if (callee && callee->authority() != rex::codegen::FunctionAuthority::IMPORT)
            todo.push_back(callee);
        }
      }
    }
    std::sort(region.members.begin(), region.members.end());
    std::sort(region.ranges.begin(), region.ranges.end());
    oracleRegions.push_back(std::move(region));
  }
  if (!oracleRegions.empty()) {
    auto& overridden = config().hostOverriddenFunctions;
    for (const auto& region : oracleRegions)
      overridden.push_back(region.root);
    std::sort(overridden.begin(), overridden.end());
    overridden.erase(std::unique(overridden.begin(), overridden.end()), overridden.end());
  }

  if (!referenceSet.empty()) {
    auto& overridden = config().hostOverriddenFunctions;
    overridden.insert(overridden.end(), referenceSet.begin(), referenceSet.end());
    std::sort(overridden.begin(), overridden.end());
    overridden.erase(std::unique(overridden.begin(), overridden.end()), overridden.end());
  }
  RecompilerConfig referenceConfig = config();
  referenceConfig.crRegistersAsLocalVariables = false;
  referenceConfig.ctrAsLocalVariable = false;
  referenceConfig.xerAsLocalVariable = false;
  referenceConfig.reservedRegisterAsLocalVariable = false;
  referenceConfig.nonArgumentRegistersAsLocalVariables = false;
  referenceConfig.nonVolatileRegistersAsLocalVariables = false;
  referenceConfig.skipLr = false;
  referenceConfig.foldRodataLoads = false;
  referenceConfig.hostLocalRegisterSaves = false;
  referenceConfig.hostLocalLr = false;
  referenceConfig.registerLocalsFunctions.clear();
  referenceConfig.fixedFpModeFunctions.clear();
  referenceConfig.singlePrecisionFunctions.clear();
  referenceConfig.directCallsBypassThunk = false;
  referenceConfig.callProbes = false;
  referenceConfig.abiPoisonChecks = false;
  EmitContext referenceCtx{binary(), referenceConfig, graph(),
                           static_cast<uint32_t>(analysisState().entryPoint), emitCtx.resolver};
  referenceCtx.referenced = &referenced;
  referenceCtx.namePrefix = "ref_";
  std::vector<std::pair<uint32_t, std::string>> referenceCopies;
  size_t uncheckedReferenceStores = 0;
  static const char* const kStoreMacros[] = {
      "REX_STORE_U8",       "REX_STORE_U16",       "REX_STORE_U32",       "REX_STORE_U64",
      "REX_STACK_STORE_U8", "REX_STACK_STORE_U16", "REX_STACK_STORE_U32", "REX_STACK_STORE_U64"};
  static const char* const kRefMacros[] = {"REX_REF_STORE_U8",  "REX_REF_STORE_U16",
                                           "REX_REF_STORE_U32", "REX_REF_STORE_U64",
                                           "REX_REF_STORE_U8",  "REX_REF_STORE_U16",
                                           "REX_REF_STORE_U32", "REX_REF_STORE_U64"};
  // The two logged copies of every region function, emitted ahead so that a
  // region with a function the copies cannot carry is dropped whole: its
  // copies call each other, so a partial region would not link.
  std::unordered_set<uint32_t> oracleMembers;    // calls that stay in the copies
  std::unordered_set<uint32_t> oracleCopied;     // functions that get copies
  for (const auto& region : oracleRegions) {
    oracleMembers.insert(region.members.begin(), region.members.end());
    oracleCopied.insert(region.members.begin(), region.members.end());
    for (const auto& [address, crtName] : region.crt)
      oracleMembers.insert(address);
  }
  struct OracleCopy {
    std::string code;
    std::unordered_set<std::string> referenced;
  };
  std::unordered_map<uint32_t, OracleCopy> oracleCopies;
  if (!oracleMembers.empty()) {
    RecompilerConfig oracleReferenceConfig = referenceConfig;
    EmitContext oracleReference{binary(), oracleReferenceConfig, graph(),
                                static_cast<uint32_t>(analysisState().entryPoint),
                                emitCtx.resolver};
    oracleReference.namePrefix = "orr_";
    oracleReference.oraclePrefix = "orr_";
    oracleReference.oracleMembers = &oracleMembers;
    EmitContext oracleCandidate{binary(), config(), graph(),
                                static_cast<uint32_t>(analysisState().entryPoint),
                                emitCtx.resolver};
    oracleCandidate.namePrefix = "orc_";
    oracleCandidate.oraclePrefix = "orc_";
    oracleCandidate.oracleMembers = &oracleMembers;
    oracleCandidate.oracleCandidate = true;
    std::unordered_set<uint32_t> blockedMembers;
    for (const auto* fn : functions) {
      const uint32_t address = static_cast<uint32_t>(fn->base());
      if (!oracleCopied.count(address))
        continue;
      OracleCopy copy;
      std::unordered_set<std::string> copyReferenced;
      for (EmitContext* octx : {&oracleReference, &oracleCandidate}) {
        bool blocked = false;
        octx->referenced = &copyReferenced;
        octx->referenceBlocked = &blocked;
        std::string body = fn->emitCpp(*octx);
        octx->referenceBlocked = nullptr;
        octx->referenced = nullptr;
        if (blocked) {
          blockedMembers.insert(address);
          break;
        }
        copy.code += fmt::format("\n// Region oracle copy ({}, rex/ppc/oracle.h)\n",
                                 octx->oracleCandidate ? "candidate" : "reference");
        for (size_t i = 0; i < 8; ++i)
          copy.code += fmt::format(
              "#pragma push_macro(\"{}\")\n#undef {}\n#define {}(x, y) {}(x, y)\n",
              kStoreMacros[i], kStoreMacros[i], kStoreMacros[i], kRefMacros[i]);
        copy.code +=
            "#pragma push_macro(\"REX_CALL_INDIRECT_FUNC\")\n#undef REX_CALL_INDIRECT_FUNC\n"
            "#define REX_CALL_INDIRECT_FUNC(x) REX_ORACLE_OPAQUE(ctx, base, (x), 0u)\n";
        copy.code += body;
        copy.code += "#pragma pop_macro(\"REX_CALL_INDIRECT_FUNC\")\n";
        for (size_t i = 0; i < 8; ++i)
          copy.code += fmt::format("#pragma pop_macro(\"{}\")\n", kStoreMacros[i]);
      }
      copy.referenced = std::move(copyReferenced);
      oracleCopies.emplace(address, std::move(copy));
    }
    if (!blockedMembers.empty()) {
      std::vector<OracleRegion> kept;
      for (auto& region : oracleRegions) {
        bool ok = true;
        for (uint32_t member : region.members) {
          if (blockedMembers.count(member)) {
            REXCODEGEN_ERROR("oracle_regions: dropping 0x{:08X}: 0x{:08X} has an instruction a "
                             "region copy cannot carry (an atomic, a hook, an unlogged store)",
                             region.root, member);
            ok = false;
            break;
          }
        }
        if (ok)
          kept.push_back(std::move(region));
      }
      oracleRegions = std::move(kept);
      std::unordered_set<uint32_t> keptMembers;
      for (const auto& region : oracleRegions)
        keptMembers.insert(region.members.begin(), region.members.end());
      std::erase_if(oracleCopies, [&](const auto& entry) { return !keptMembers.count(entry.first); });
    }
  }

  // Native regions (native_regions, #750): two inline sets per root, both
  // with the candidate options, appended to the root's unit. `orn_` is the
  // oracle's candidate (logged stores, opaque boundaries); `nrg_` makes its
  // outside calls for real and runs as `nr_sub_X`.
  struct NativeBlock {
    std::string code;
    std::unordered_set<std::string> referenced;
  };
  std::unordered_map<uint32_t, NativeBlock> nativeBlocks;
  std::vector<const OracleRegion*> nativeRegions;
  for (uint32_t nativeRoot : config().nativeRegions) {
    const auto region = std::find_if(oracleRegions.begin(), oracleRegions.end(),
                                     [&](const OracleRegion& r) { return r.root == nativeRoot; });
    if (region == oracleRegions.end()) {
      REXCODEGEN_ERROR("native_regions: 0x{:08X} is not a kept oracle_regions root", nativeRoot);
      continue;
    }
    // The copies are always-inline, so the region must not recurse.
    std::unordered_set<uint32_t> members(region->members.begin(), region->members.end());
    std::unordered_map<uint32_t, int> state;  // 1 visiting, 2 done
    bool cyclic = false;
    std::function<void(uint32_t)> visit = [&](uint32_t address) {
      state[address] = 1;
      const auto* fn = graph().getFunction(address);
      for (const auto* edges : {&fn->calls(), &fn->tailCalls()}) {
        for (const auto& edge : *edges) {
          if (!edge.target.isFunction())
            continue;
          const auto* callee = edge.target.asFunction();
          const uint32_t next = callee ? static_cast<uint32_t>(callee->base()) : 0;
          if (!callee || !members.count(next))
            continue;
          if (state[next] == 1)
            cyclic = true;
          else if (state[next] == 0)
            visit(next);
        }
      }
      state[address] = 2;
    };
    visit(nativeRoot);
    if (cyclic) {
      REXCODEGEN_ERROR("native_regions: 0x{:08X} recurses; its copies cannot be inlined",
                       nativeRoot);
      continue;
    }
    NativeBlock block;
    bool ok = true;
    for (const std::string prefix : {"orn_", "nrg_"}) {
      const bool oracleVariant = prefix == "orn_";
      EmitContext nctx{binary(), config(), graph(),
                       static_cast<uint32_t>(analysisState().entryPoint), emitCtx.resolver};
      nctx.namePrefix = prefix;
      nctx.oraclePrefix = prefix;
      nctx.oracleMembers = oracleVariant ? &oracleMembers : &members;
      nctx.oracleCandidate = true;
      nctx.regionRealCalls = !oracleVariant;
      std::unordered_set<std::string> refs;
      nctx.referenced = &refs;
      std::string copies;
      for (uint32_t member : region->members)
        copies += graph().getFunction(member)->emitCpp(nctx);
      if (copies.find("ppc_setjmp") != std::string::npos ||
          copies.find("ppc_longjmp") != std::string::npos) {
        REXCODEGEN_ERROR("native_regions: 0x{:08X} reaches setjmp/longjmp", nativeRoot);
        ok = false;
        break;
      }
      std::vector<std::string> names;
      const std::string body = NativeRegionTransform(copies, prefix, region->name, names);
      std::string part = fmt::format("\n// Native region 0x{:08X}{} (rex/ppc/oracle.h)\n",
                                     nativeRoot, oracleVariant ? ", oracle candidate" : "");
      if (oracleVariant) {
        for (size_t i = 0; i < 8; ++i)
          part += fmt::format("#pragma push_macro(\"{}\")\n#undef {}\n#define {}(x, y) {}(x, y)\n",
                              kStoreMacros[i], kStoreMacros[i], kStoreMacros[i], kRefMacros[i]);
        part +=
            "#pragma push_macro(\"REX_CALL_INDIRECT_FUNC\")\n#undef REX_CALL_INDIRECT_FUNC\n"
            "#define REX_CALL_INDIRECT_FUNC(x) REX_ORACLE_OPAQUE(ctx, base, (x), 0u)\n";
      }
      for (const auto& name : names)
        part += fmt::format("REX_NR_FN({});\n", name);
      if (oracleVariant) {
        part += body;
      } else {
        // The region's context is a host local the compiler keeps in
        // registers; copying the whole PPCContext into it and back (about
        // 1 KB a call, the vector file included) cost more than it saved
        // on arm64. Copy only the fields the region names, plus the small
        // special registers: the others never change inside the region, so
        // the caller's context stays current for them.
        std::set<std::string> fields{"lr",  "ctr", "xer", "reserved", "msr", "cr0",
                                     "cr1", "cr2", "cr3", "cr4",      "cr5", "cr6",
                                     "cr7", "fpscr", "vscr_sat"};
        // Fields the region may write: assigned (plain or compound), stepped,
        // passed to a store intrinsic, address taken or given a method call
        // (fpscr's mode switches). Only these can differ from the caller's
        // context, so only these are published before a call and at the exit;
        // reloads after a call take every field the region uses.
        std::set<std::string> written{"lr", "ctr", "xer", "reserved", "cr0", "cr1", "cr2",
                                      "cr3", "cr4", "cr5", "cr6", "cr7", "fpscr", "vscr_sat"};
        auto ident = [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        };
        static constexpr std::string_view kField = "ctx.";
        for (size_t at = body.find(kField); at != std::string::npos;
             at = body.find(kField, at + 1)) {
          if (at > 0 && (ident(body[at - 1]) || body[at - 1] == '.'))
            continue;
          size_t end = at + kField.size();
          while (end < body.size() && ident(body[end]))
            ++end;
          const std::string field = body.substr(at + kField.size(), end - at - kField.size());
          fields.insert(field);
          // Past member and index selectors.
          size_t after = end;
          bool method = false;
          while (after < body.size()) {
            if (body[after] == '.') {
              size_t next = after + 1;
              while (next < body.size() && ident(body[next]))
                ++next;
              if (next < body.size() && body[next] == '(') {
                method = true;
                break;
              }
              after = next;
            } else if (body[after] == '[') {
              const size_t close = body.find(']', after);
              if (close == std::string::npos)
                break;
              after = close + 1;
            } else {
              break;
            }
          }
          const std::string_view tail = std::string_view(body).substr(after, 5);
          const bool assigned =
              (tail.starts_with(" = ") ) || tail.starts_with(" += ") || tail.starts_with(" -= ") ||
              tail.starts_with(" |= ") || tail.starts_with(" &= ") || tail.starts_with(" ^= ") ||
              tail.starts_with(" *= ") || tail.starts_with(" /= ") || tail.starts_with(" <<= ") ||
              tail.starts_with(" >>= ") || tail.starts_with("++") || tail.starts_with("--");
          const bool prefixed =
              at > 0 && (body[at - 1] == '&' ||
                         (at > 1 && (body.compare(at - 2, 2, "++") == 0 ||
                                     body.compare(at - 2, 2, "--") == 0)));
          bool stored = false;
          if (const size_t open = body.rfind('(', at);
              open != std::string::npos && at - open < 8) {
            size_t name = open;
            while (name > 0 && ident(body[name - 1]))
              --name;
            stored = body.substr(name, open - name).find("store") != std::string::npos;
          }
          if (assigned || prefixed || stored || method)
            written.insert(field);
        }
        auto emitSync = [&](const std::string& name, const std::set<std::string>& set) {
          part += fmt::format(
              "static inline __attribute__((always_inline)) void {}(PPCContext& __restrict to, "
              "const PPCContext& __restrict from) {{\n",
              name);
          // FPRs are copied as doubles (a register move keeps the bits), so
          // the copy does not reintroduce the integer view either.
          for (const auto& field : set) {
            if (!fields.count(field))
              continue;
            const bool fpr = field.size() > 1 && field[0] == 'f' &&
                             std::all_of(field.begin() + 1, field.end(), [](char c) {
                               return std::isdigit(static_cast<unsigned char>(c));
                             });
            part += fpr ? fmt::format("\tto.{0}.f64 = from.{0}.f64;\n", field)
                        : fmt::format("\tto.{0} = from.{0};\n", field);
          }
          part += "}\n";
        };
        const std::string sync = "nrs_" + region->name;
        const std::string publish = "nrp_" + region->name;
        emitSync(sync, fields);
        emitSync(publish, written);
        std::string synced = body;
        // The host CRT's memcpy, memmove and memset read r3-r5 and set r3.
        for (const char* crt : {"rexcrt_memcpy", "rexcrt_memmove", "rexcrt_memset"}) {
          const std::string from =
              fmt::format("{{ real = ctx; {}(real, base); ctx = real;", crt);
          const std::string to = fmt::format(
              "{{ real.r3 = ctx.r3; real.r4 = ctx.r4; real.r5 = ctx.r5; {}(real, base); "
              "ctx.r3 = real.r3;",
              crt);
          for (size_t at = synced.find(from); at != std::string::npos;
               at = synced.find(from, at + to.size()))
            synced.replace(at, from.size(), to);
        }
        for (const auto& [from, to] :
             {std::pair<std::string, std::string>{"real = ctx;", publish + "(real, ctx);"},
              std::pair<std::string, std::string>{"ctx = real;", sync + "(ctx, real);"}}) {
          for (size_t at = synced.find(from); at != std::string::npos;) {
            // An indirect call declares its own `PPCContext& __restrict ctx
            // = real;`: that is a binding, not a copy back.
            static constexpr std::string_view kBinding = "__restrict ";
            if (at >= kBinding.size() &&
                std::string_view(synced).substr(at - kBinding.size(), kBinding.size()) ==
                    kBinding) {
              at = synced.find(from, at + from.size());
              continue;
            }
            synced.replace(at, from.size(), to);
            at = synced.find(from, at + to.size());
          }
        }
        // The region is one host function, so its guest memory accesses need
        // not be volatile: the compiler may keep an inlined member's spills
        // and register saves in host registers and drop a load another
        // member repeats. Outside calls still see memory, as they take
        // `base`; within the region the oracle checks the stores
        // (REX_PLAIN_STACK does this for the stack of the whole build).
        static constexpr const char* kPlainMemory[] = {
            "REX_STACK_QUAL",
            "REX_LOAD_U8(x) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))",
            "REX_LOAD_U16(x) __builtin_bswap16(*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))",
            "REX_LOAD_U32(x) __builtin_bswap32(*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))",
            "REX_LOAD_U64(x) __builtin_bswap64(*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))",
            "REX_STORE_U8(x, y) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))",
            "REX_STORE_U16(x, y) (*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = "
            "__builtin_bswap16(y))",
            "REX_STORE_U32(x, y) (*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = "
            "__builtin_bswap32(y))",
            "REX_STORE_U64(x, y) (*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = "
            "__builtin_bswap64(y))",
        };
        auto macroName = [](std::string_view definition) {
          return std::string(definition.substr(0, definition.find_first_of("( ")));
        };
        for (const char* definition : kPlainMemory)
          part += fmt::format("#pragma push_macro(\"{0}\")\n#undef {0}\n#define {1}\n",
                              macroName(definition), definition);
        part += synced;
        for (const char* definition : kPlainMemory)
          part += fmt::format("#pragma pop_macro(\"{}\")\n", macroName(definition));
        part += fmt::format(
            "extern \"C\" REX_FUNC(nr_{0}) {{\n\tREX_FUNC_PROLOGUE();\n\tPPCContext local;\n"
            "\t{1}(local, ctx);\n\t{2}{0}(local, base, ctx);\n\t{3}(ctx, local);\n}}\n",
            region->name, sync, prefix, publish);
        block.code += part;
        for (const auto& ref : refs)
          if (!IsNativeRegionName(ref, prefix))
            block.referenced.insert(ref);
        continue;
      }
      if (oracleVariant) {
        part += "#pragma pop_macro(\"REX_CALL_INDIRECT_FUNC\")\n";
        for (size_t i = 0; i < 8; ++i)
          part += fmt::format("#pragma pop_macro(\"{}\")\n", kStoreMacros[i]);
      }
      part += fmt::format(
          "extern \"C\" REX_FUNC({}{}) {{\n\tREX_FUNC_PROLOGUE();\n\tPPCContext local = ctx;\n"
          "\t{}{}(local, base, ctx);\n\tctx = local;\n}}\n",
          oracleVariant ? "orn_root_" : "nr_", region->name, prefix, region->name);
      block.code += part;
      for (const auto& ref : refs)
        if (!IsNativeRegionName(ref, prefix))
          block.referenced.insert(ref);
    }
    if (!ok)
      continue;
    nativeBlocks.emplace(nativeRoot, std::move(block));
    nativeRegions.push_back(&*region);
  }

  for (const auto* fn : functions) {
    referenced.clear();
    std::string code = fn->emitCpp(emitCtx);
    if (auto native = nativeBlocks.find(static_cast<uint32_t>(fn->base()));
        native != nativeBlocks.end()) {
      code += native->second.code;
      referenced.insert(native->second.referenced.begin(), native->second.referenced.end());
    }
    if (auto copy = oracleCopies.find(static_cast<uint32_t>(fn->base()));
        copy != oracleCopies.end()) {
      code += copy->second.code;
      referenced.insert(copy->second.referenced.begin(), copy->second.referenced.end());
    }
    if (referenceSet.count(static_cast<uint32_t>(fn->base())) &&
        fn->authority() != rex::codegen::FunctionAuthority::IMPORT) {
      bool blocked = false;
      referenceCtx.referenceBlocked = &blocked;
      std::string reference = fn->emitCpp(referenceCtx);
      referenceCtx.referenceBlocked = nullptr;
      if (blocked) {
        REXCODEGEN_DEBUG("Reference copy of 0x{:08X} dropped: an instruction the verifier cannot "
                         "roll back", fn->base());
      } else {
        // Under host_local_lr the optimized function keeps the link register
        // in a local and leaves the caller's LR slot unwritten, which nothing
        // reads (the plan keeps the slot wherever something might). The copy
        // still writes it; compare everything else.
        // The same holds for the GPR save area under host_local_register_saves
        // and register_locals_functions: see UncheckSaveAreaStores.
        uncheckedReferenceStores += UncheckSaveAreaStores(reference, code);
        std::string wrapped = "\n// Reference copy for the lockstep verifier (rex/ppc/reference.h)\n";
        for (size_t i = 0; i < 8; ++i)
          wrapped += fmt::format("#pragma push_macro(\"{}\")\n#undef {}\n#define {}(x, y) {}(x, y)\n",
                                 kStoreMacros[i], kStoreMacros[i], kStoreMacros[i], kRefMacros[i]);
        wrapped += reference;
        for (size_t i = 0; i < 8; ++i)
          wrapped += fmt::format("#pragma pop_macro(\"{}\")\n", kStoreMacros[i]);
        code += wrapped;
        referenceCopies.emplace_back(static_cast<uint32_t>(fn->base()), fn->name());
      }
    }
    if (code.size() > maxFileBytes) {
      REXCODEGEN_WARN("Function 0x{:08X} is {} bytes, exceeds max_file_size_bytes ({})", fn->base(),
                      code.size(), maxFileBytes);
    }
    sizes.push_back({static_cast<uint32_t>(fn->base()), code.size()});
    bodies.push_back(std::move(code));
    references.push_back(referenced);
  }
  emitCtx.referenced = nullptr;
  emitCtx.foldedRanges = nullptr;
  if (config().hostLocalRegisterSaves)
    rex::codegen::LogHostLocalSaveStats();
  if (config().hostLocalLr)
    rex::codegen::LogHostLocalLrStats();
  if (!config().registerLocalsFunctions.empty() || !config().fixedFpModeFunctions.empty() ||
      !config().singlePrecisionFunctions.empty())
    rex::codegen::LogSpecializedFunctionStats();

  // The reference functions, for the host verifier: one macro line each.
  {
    println("// Generated by rexglue: the functions emitted with a reference copy.");
    println("// The includer defines REX_REFERENCE_FUNCTION(address, name).");
    println("#pragma once\n");
    println("#define REX_REFERENCE_COUNT {}", referenceCopies.size());
    for (const auto& [address, name] : referenceCopies)
      println("REX_REFERENCE_FUNCTION(0x{:08X}, {})", address, name);
    SaveCurrentOutData(fmt::format("{}_reference.h", projectName));
    REXCODEGEN_TRACE("Emitted {} reference copies", referenceCopies.size());
    if (uncheckedReferenceStores)
      REXCODEGEN_INFO("reference copies: {} register save-area stores the optimized functions "
                      "keep in host locals are rolled back but not compared",
                      uncheckedReferenceStores);
  }

  // The region oracle's roots and the code each region spans, for the host
  // oracle and its interpreter.
  {
    println("// Generated by rexglue: region oracle roots (oracle_regions, rex/ppc/oracle.h).");
    println("// The includer defines REX_ORACLE_REGION(address, name) and");
    println("// REX_ORACLE_RANGE(root, start, end), the code of the region's functions, and");
    println("// REX_ORACLE_CRT(root, address, name), a host CRT function the region calls.");
    println("#pragma once\n");
    println("#define REX_ORACLE_REGION_COUNT {}", oracleRegions.size());
    for (const auto& region : oracleRegions) {
      println("REX_ORACLE_REGION(0x{:08X}, {})", region.root, region.name);
      for (const auto& [start, end] : region.ranges)
        println("REX_ORACLE_RANGE(0x{:08X}, 0x{:08X}, 0x{:08X})", region.root, start, end);
      for (const auto& [address, crtName] : region.crt)
        println("REX_ORACLE_CRT(0x{:08X}, 0x{:08X}, {})", region.root, address, crtName);
    }
    // Roots with a native region: `nr_<name>` runs it, `orn_root_<name>` is
    // the oracle's candidate for it.
    println("#ifdef REX_ORACLE_NATIVE");
    for (const auto* region : nativeRegions)
      println("REX_ORACLE_NATIVE(0x{:08X}, {})", region->root, region->name);
    println("#endif");
    SaveCurrentOutData(fmt::format("{}_oracle.h", projectName));
    for (const auto* region : nativeRegions)
      REXCODEGEN_INFO("native region 0x{:08X}: {} functions inlined", region->root,
                      region->members.size());
    for (const auto& region : oracleRegions)
      REXCODEGEN_INFO("oracle region 0x{:08X}: {} functions with copies, {} code ranges",
                      region.root, region.members.size(), region.ranges.size());
  }

  // The read-only bytes folded into literals, with their FNV-1a 64 hash for the
  // runtime's check against the loaded image (rex/ppc/func.h PPCFoldedRange).
  {
    uint64_t hash = 14695981039346656037ull;
    size_t bytes = 0;
    println("#include \"{}_pch.h\"\n", projectName);
    println("const PPCFoldedRange PPCFoldedRanges[] = {{");
    for (const auto& [address, size] : foldedRanges) {
      const auto* section = binary().findSection(address);
      const uint8_t* data = section ? section->translate(address) : nullptr;
      if (!data)
        continue;
      for (uint32_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
      }
      bytes += size;
      println("	{{ 0x{:08X}, {} }},", address, size);
    }
    println("	{{ 0, 0 }}");
    println("}};");
    println("const uint64_t PPCFoldedHash = 0x{:016X}ull;", hash);
    std::map<uint32_t, uint32_t> jumpTables;
    for (const auto* fn : functions) {
      for (const auto& jt : fn->jumpTables()) {
        if (!jt.tableAddress || jt.targets.empty())
          continue;
        auto& size = jumpTables[jt.tableAddress];
        size = std::max<uint32_t>(size, uint32_t(jt.targets.size()) * jt.entryBytes);
      }
    }
    println("const PPCFoldedRange PPCJumpTableRanges[] = {{");
    for (const auto& [address, size] : jumpTables)
      println("	{{ 0x{:08X}, {} }},", address, size);
    println("	{{ 0, 0 }}");
    println("}};");
    SaveCurrentOutData(fmt::format("{}_folds.cpp", projectName));
    REXCODEGEN_TRACE("Folded {} read-only ranges ({} bytes) into literals", foldedRanges.size(),
                     bytes);
  }

  auto partition = OutputPartition::Load(outputPath / kPartitionFileName);
  auto buckets = partition.Assign(sizes, maxFileBytes);

  for (size_t index = 0; index < buckets.size(); ++index) {
    // Buckets are address-ordered, so a call can precede its definition.
    std::unordered_set<std::string> needed;
    for (size_t entry : buckets[index]) {
      needed.insert(references[entry].begin(), references[entry].end());
    }

    std::vector<std::string> ordered(needed.begin(), needed.end());
    std::sort(ordered.begin(), ordered.end());
    println("#pragma once\n");
    println("#include \"{}_pch.h\"\n", projectName);
    for (const auto& name : ordered) {
      println("DECLARE_REX_FUNC({});", name);
    }
    SaveCurrentOutData(fmt::format("{}_funcs.{}.h", projectName, index));

    println("#include \"{}_funcs.{}.h\"\n", projectName, index);
    for (size_t entry : buckets[index]) {
      out += bodies[entry];
    }
    SaveCurrentOutData(fmt::format("{}_recomp.{}.cpp", projectName, index));
  }

  out = partition.Serialize();
  SaveCurrentOutData(kPartitionFileName);

  REXCODEGEN_TRACE("Recompilation complete.");

  // Generate sources.cmake
  REXCODEGEN_TRACE("Recompile: generating sources.cmake");
  {
    auto& recompFiles = tmplData["recomp_files"];
    recompFiles = nlohmann::json::array();
    recompFiles.push_back(fmt::format("{}_folds.cpp", projectName));
    for (size_t i = 0; i < buckets.size(); ++i) {
      recompFiles.push_back(fmt::format("{}_recomp.{}.cpp", projectName, i));
    }
    out = renderWithJson(registry, "codegen/sources_cmake", tmplData);
    SaveCurrentOutData("sources.cmake");
  }

  // Write all buffered files to disk
  return FlushPendingWrites();
}

void CodegenWriter::SaveCurrentOutData(const std::string_view name) {
  if (!out.empty()) {
    pendingWrites.emplace_back(std::string(name), std::move(out));
    out.clear();
  }
}

bool CodegenWriter::FlushPendingWrites() {
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;

  std::unordered_set<std::string> emitted;
  emitted.reserve(pendingWrites.size());

  // A swallowed failure would be stamped as success and skipped on the next run.
  bool ok = true;

  for (const auto& [filename, content] : pendingWrites) {
    emitted.insert(filename);
    auto filePath = outputPath / filename;

    switch (WriteIfChanged(filePath, content)) {
      case WriteOutcome::Failed:
        REXCODEGEN_ERROR("Failed to write {}", filePath.string());
        ok = false;
        break;
      case WriteOutcome::Unchanged:
        REXCODEGEN_TRACE("Unchanged, skipping write: {}", filePath.string());
        unchangedFiles_.push_back(filename);
        break;
      case WriteOutcome::Written:
        REXCODEGEN_TRACE("Wrote {} bytes to {}", content.size(), filePath.string());
        writtenFiles_.push_back(filename);
        break;
    }
  }

  for (const auto& entry : std::filesystem::directory_iterator(outputPath)) {
    if (!entry.is_regular_file())
      continue;
    auto filename = entry.path().filename().string();
    if (emitted.contains(filename))
      continue;
    if (!IsGeneratedOutputName(filename, config().projectName))
      continue;

    std::error_code ec;
    std::filesystem::remove(entry.path(), ec);
    if (ec) {
      REXCODEGEN_ERROR("Failed to delete stale output {}: {}", entry.path().string(), ec.message());
      continue;
    }
    REXCODEGEN_TRACE("Deleted stale output: {}", entry.path().string());
    deletedFiles_.push_back(filename);
  }

  pendingWrites.clear();
  return ok;
}

}  // namespace rex::codegen
