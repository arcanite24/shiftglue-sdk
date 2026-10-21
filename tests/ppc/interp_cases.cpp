// Runs the region oracle's interpreter (rex/ppc/interpreter.h) on the PPC
// instruction test corpus. It reads the same assembled binaries and symbol
// maps as ppc_tests, but parses each case's #_ directives at run time and
// executes the case with the interpreter instead of the recompiled code, so
// the two implementations are checked against the same expectations
// independently. With REXGLUE_PPC_TEST_EXTRA_ASM_DIRS pointing at xenia's
// generated corpus, those expectations are hardware recordings.
//
// A case the interpreter cannot run (an instruction outside its scope) is
// counted as unsupported, not failed: the oracle refuses such a region at
// run time the same way. Cases in skip.txt are counted apart, and so are the
// generated cases xenia-canary itself skips (canary-skip.txt beside them).
// The reciprocal estimates (fres, frsqrte, vrefp, vrsqrtefp) compute the exact
// value, as the generator does, where hardware returns a table estimate:
// their value differences are counted as estimates, not failures (a NaN
// operand still has to come back as hardware returns it).
//
// Environment: PPC_INTERP_VERBOSE=1 prints every failure (the default prints
// the first 20 per suite); PPC_INTERP_FILTER=<substring> limits the suites.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ppc/interpreter.h>
#include <rex/system/xmemory.h>

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kTestBase = 0x82010000;

rex::memory::Memory& TestMemory() {
  static rex::memory::Memory memory;
  static bool initialized = false;
  if (!initialized) {
    if (!memory.Initialize())
      throw std::runtime_error("memory initialization failed");
    if (auto* heap = memory.LookupHeap(0x10000))
      heap->AllocFixed(0x10000, 0x10000000, 0x1000,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
    initialized = true;
  }
  return memory;
}

std::string Trim(std::string s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

uint64_t ParseInt(std::string s) {
  s = Trim(s);
  while (!s.empty() && (s.back() == 'l' || s.back() == 'L' || s.back() == 'u' || s.back() == 'U'))
    s.pop_back();
  bool neg = false;
  if (!s.empty() && s[0] == '-') {
    neg = true;
    s.erase(0, 1);
  }
  uint64_t v = 0;
  if (s.starts_with("0x") || s.starts_with("0X"))
    std::from_chars(s.data() + 2, s.data() + s.size(), v, 16);
  else
    std::from_chars(s.data(), s.data() + s.size(), v, 10);
  return neg ? uint64_t(-int64_t(v)) : v;
}

struct Reg {
  std::string name;
  enum Kind { kGpr, kFloatBits, kFloatValue, kVector, kCr } kind = kGpr;
  uint64_t bits = 0;
  double value = 0;
  uint32_t words[4] = {};  // guest order, high word first
};

struct Mem {
  uint32_t address = 0;
  std::vector<uint8_t> bytes;
};

struct Case {
  std::string label;
  std::vector<Reg> in, out;
  std::vector<Mem> mem_in, mem_out;
};

Reg ParseReg(const std::string& rest) {
  Reg r;
  std::istringstream ss(rest);
  ss >> r.name;
  std::string value;
  std::getline(ss, value);
  value = Trim(value);
  if (r.name == "cr") {
    r.kind = Reg::kCr;
    r.bits = ParseInt(value);
  } else if (r.name[0] == 'v') {
    r.kind = Reg::kVector;
    std::string inner = value.substr(value.find('[') + 1);
    inner = inner.substr(0, inner.find(']'));
    std::istringstream ws(inner);
    std::string w;
    for (int i = 0; i < 4 && std::getline(ws, w, ','); ++i)
      r.words[i] = uint32_t(ParseInt("0x" + Trim(w)));
  } else if (r.name[0] == 'f') {
    if (value.size() > 2 && value.front() == '[') {
      r.kind = Reg::kFloatBits;
      r.bits = ParseInt("0x" + value.substr(1, value.size() - 2));
    } else if (value.find('.') != std::string::npos) {
      r.kind = Reg::kFloatValue;
      r.value = std::strtod(value.c_str(), nullptr);
    } else {
      r.kind = Reg::kFloatBits;
      r.bits = ParseInt(value);
    }
  } else {
    r.kind = Reg::kGpr;
    r.bits = ParseInt(value);
  }
  return r;
}

Mem ParseMem(const std::string& rest) {
  Mem m;
  std::istringstream ss(rest);
  std::string address;
  ss >> address;
  m.address = uint32_t(ParseInt(address.starts_with("0x") ? address : "0x" + address));
  std::string bytes;
  std::getline(ss, bytes);
  std::erase_if(bytes, [](char c) { return c == '[' || c == ']' || c == ','; });
  // Bytes are hex pairs, spaced or not (`10001000 ABCDEF12`).
  std::erase_if(bytes, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
  for (size_t i = 0; i + 1 < bytes.size(); i += 2)
    m.bytes.push_back(uint8_t(ParseInt("0x" + bytes.substr(i, 2))));
  return m;
}

// The case format of tests/ppc/README.md: a label, input directives, the
// code, output directives. Comments in /* */ are not used by the corpus.
std::vector<Case> ParseCases(const fs::path& path) {
  // As the recompile-tests generator reads them: a label inside a case's
  // code (a branch target) belongs to that case until its outputs begin.
  std::vector<Case> cases;
  std::ifstream in(path);
  std::string line;
  Case* current = nullptr;
  bool in_outputs = false;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty())
      continue;
    if (line.starts_with("#_")) {
      if (!current)
        continue;
      const std::string body = Trim(line.substr(2));
      auto take = [&](const char* token, auto& regs, auto& mems, bool is_in) {
        const std::string reg_token = std::string("REGISTER_") + token;
        const std::string mem_token = std::string("MEMORY_") + token;
        if (body.starts_with(reg_token))
          regs.push_back(ParseReg(body.substr(reg_token.size())));
        else if (body.starts_with(mem_token))
          mems.push_back(ParseMem(body.substr(mem_token.size())));
        (void)is_in;
      };
      if (body.starts_with("REGISTER_IN") || body.starts_with("MEMORY_IN")) {
        take("IN", current->in, current->mem_in, true);
      } else {
        take("OUT", current->out, current->mem_out, false);
        in_outputs = true;
      }
      continue;
    }
    if (line[0] == '#')
      continue;
    const auto colon = line.find(':');
    if (colon != std::string::npos && line.find(' ') > colon && line.find('\t') > colon &&
        colon + 1 == line.size()) {
      if (current && !in_outputs)
        continue;
      cases.push_back({});
      current = &cases.back();
      current->label = line.substr(0, colon);
      in_outputs = false;
    }
  }
  return cases;
}

std::unordered_map<std::string, uint32_t> ParseMap(const fs::path& path) {
  std::unordered_map<std::string, uint32_t> symbols;
  std::ifstream in(path);
  std::string offset, type, name;
  while (in >> offset >> type >> name)
    symbols[name] = kTestBase + uint32_t(ParseInt("0x" + offset));
  return symbols;
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// "<suite>.<case>" as the recompile-tests generator names a case.
std::string CaseName(const std::string& stem, const std::string& label) {
  if (stem.starts_with("instr_")) {
    const std::string group = stem.substr(6);
    const std::string prefix = "test_" + group + "_";
    if (label.starts_with(prefix))
      return group + ".test_" + label.substr(prefix.size());
    if (label == "test_" + group)
      return group + ".test";
    return group + "." + label;
  }
  if (stem.starts_with("seq_")) {
    if (label.starts_with("seq_"))
      return "seq.test_" + label.substr(4);
    return "seq." + label;
  }
  return stem + "." + label;
}

std::set<std::string> ReadList(const fs::path& path) {
  std::set<std::string> names;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (auto hash = line.find('#'); hash != std::string::npos)
      line = line.substr(0, hash);
    line = Trim(line);
    if (!line.empty())
      names.insert(line);
  }
  return names;
}

struct Code {
  const std::vector<uint8_t>* image;
};

bool Fetch(void* user, uint32_t address, uint32_t* insn) {
  const auto& image = *static_cast<Code*>(user)->image;
  if (address < kTestBase || address - kTestBase + 4 > image.size())
    return false;
  const uint8_t* p = image.data() + (address - kTestBase);
  *insn = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  return true;
}

uint32_t CrWord(const PPCContext& ctx) {
  const PPCCRRegister* cr = &ctx.cr0;
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= cr[i].raw() << (28 - 4 * i);
  return v;
}

PPCRegister* Gpr(PPCContext& ctx, int n) {
  switch (n) {
    case 0: return &ctx.r0;
    case 1: return &ctx.r1;
    case 2: return &ctx.r2;
    case 3: return &ctx.r3;
    default: return &ctx.r4 + (n - 4);
  }
}

// Empty when the case passes, else the first difference.
std::string RunCase(const Case& c, uint32_t entry, Code& code, rex::ppc::interp::Exit* exit) {
  uint8_t* memory = TestMemory().virtual_membase();
  PPCContext ctx{};
  ctx.fpscr.loadFromHost();
  for (const Reg& r : c.in) {
    const int n = std::atoi(r.name.c_str() + 1);
    switch (r.kind) {
      case Reg::kCr:
        for (int i = 0; i < 8; ++i)
          (&ctx.cr0)[i].set_raw(uint32_t(r.bits >> (28 - 4 * i)) & 15);
        break;
      case Reg::kVector:
        for (int i = 0; i < 4; ++i)
          (&ctx.v0)[n].u32[3 - i] = r.words[i];
        break;
      case Reg::kFloatBits:
        (&ctx.f0)[n].u64 = r.bits;
        break;
      case Reg::kFloatValue:
        (&ctx.f0)[n].f64 = r.value;
        break;
      case Reg::kGpr:
        if (r.name[0] == 'r')
          Gpr(ctx, n)->u64 = r.bits;
        else
          return "unknown register " + r.name;
        break;
    }
  }
  for (const Mem& m : c.mem_in)
    for (size_t i = 0; i < m.bytes.size(); ++i)
      memory[m.address + i] = m.bytes[i];

  rex::ppc::interp::Config config;
  config.fetch = Fetch;
  config.fetch_user = &code;
  config.max_steps = 1'000'000;
  rex::ppc::RefLogBegin();
  const auto result = rex::ppc::interp::Run(ctx, memory, entry, config);
  rex::ppc::RefLogBegin();
  *exit = result.exit;
  if (result.exit != rex::ppc::interp::Exit::kReturned) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s at %08X (%08X)", rex::ppc::interp::ExitName(result.exit),
                  result.pc, result.insn);
    return buf;
  }

  char buf[160];
  for (const Reg& r : c.out) {
    const int n = std::atoi(r.name.c_str() + 1);
    switch (r.kind) {
      case Reg::kCr:
        if (CrWord(ctx) != uint32_t(r.bits)) {
          std::snprintf(buf, sizeof buf, "cr %08X expected %08X", CrWord(ctx), uint32_t(r.bits));
          return buf;
        }
        break;
      case Reg::kVector:
        for (int i = 0; i < 4; ++i) {
          const uint32_t got = (&ctx.v0)[n].u32[3 - i];
          if (got != r.words[i]) {
            std::snprintf(buf, sizeof buf, "%s[%d] %08X expected %08X", r.name.c_str(), i, got,
                          r.words[i]);
            return buf;
          }
        }
        break;
      case Reg::kFloatBits:
        if ((&ctx.f0)[n].u64 != r.bits) {
          std::snprintf(buf, sizeof buf, "%s %016llX expected %016llX", r.name.c_str(),
                        (unsigned long long)(&ctx.f0)[n].u64, (unsigned long long)r.bits);
          return buf;
        }
        break;
      case Reg::kFloatValue:
        if ((&ctx.f0)[n].f64 != r.value) {
          std::snprintf(buf, sizeof buf, "%s %g expected %g", r.name.c_str(), (&ctx.f0)[n].f64,
                        r.value);
          return buf;
        }
        break;
      case Reg::kGpr:
        if (Gpr(ctx, n)->u64 != r.bits) {
          std::snprintf(buf, sizeof buf, "%s %016llX expected %016llX", r.name.c_str(),
                        (unsigned long long)Gpr(ctx, n)->u64, (unsigned long long)r.bits);
          return buf;
        }
        break;
    }
  }
  for (const Mem& m : c.mem_out)
    for (size_t i = 0; i < m.bytes.size(); ++i)
      if (memory[m.address + i] != m.bytes[i]) {
        std::snprintf(buf, sizeof buf, "memory %08X %02X expected %02X", unsigned(m.address + i),
                      memory[m.address + i], m.bytes[i]);
        return buf;
      }
  return {};
}

std::vector<std::string> Split(const std::string& list) {
  std::vector<std::string> out;
  std::stringstream ss(list);
  std::string item;
  while (std::getline(ss, item, '|'))
    if (!item.empty())
      out.push_back(item);
  return out;
}

}  // namespace

namespace {
bool IsEstimateSuite(const std::string& stem) {
  for (const char* op : {"fres", "frsqrte", "vrefp", "vrsqrtefp"})
    if (stem.ends_with(std::string("_") + op) || stem.ends_with(std::string("_") + op + "128"))
      return true;
  return false;
}
}  // namespace

TEST_CASE("interpreter runs the instruction corpus", "[ppc][interpreter]") {
  const fs::path bin_dir = PPC_INTERP_BIN_DIR;
  const std::set<std::string> skips = ReadList(PPC_INTERP_SKIP_FILE);
  const bool verbose = std::getenv("PPC_INTERP_VERBOSE") != nullptr;
  const char* filter = std::getenv("PPC_INTERP_FILTER");

  uint64_t total = 0, passed = 0, failed = 0, unsupported = 0;
  uint64_t skip_listed = 0, skip_listed_passing = 0, canary_listed = 0, canary_failed = 0;
  uint64_t estimates = 0;
  std::map<std::string, uint64_t> failures_by_suite, unsupported_by_suite;

  for (const std::string& dir : Split(PPC_INTERP_ASM_DIRS)) {
    std::set<std::string> canary_skips;
    if (fs::exists(fs::path(dir) / "canary-skip.txt"))
      canary_skips = ReadList(fs::path(dir) / "canary-skip.txt");
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir))
      if (entry.path().extension() == ".s")
        files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
      const std::string stem = file.stem().string();
      if (filter && stem.find(filter) == std::string::npos)
        continue;
      const auto image = ReadFile(bin_dir / (stem + ".bin"));
      const auto symbols = ParseMap(bin_dir / (stem + ".map"));
      Code code{&image};
      int printed = 0;
      for (const Case& c : ParseCases(file)) {
        const auto sym = symbols.find(c.label);
        if (sym == symbols.end() || (c.in.empty() && c.out.empty() && c.mem_out.empty()))
          continue;
        ++total;
        const std::string name = CaseName(stem, c.label);
        const bool skip = skips.count(name) != 0;
        const std::string canary_label =
            c.label.starts_with("test_") ? c.label.substr(5) : c.label;
        const bool canary = canary_skips.count(canary_label) != 0;
        rex::ppc::interp::Exit exit{};
        const std::string why = RunCase(c, sym->second, code, &exit);
        if (exit == rex::ppc::interp::Exit::kUnsupported) {
          ++unsupported;
          ++unsupported_by_suite[stem];
          continue;
        }
        if (skip) {
          ++skip_listed;
          skip_listed_passing += why.empty();
          continue;
        }
        if (canary) {
          ++canary_listed;
          canary_failed += !why.empty();
          if (!why.empty())
            continue;
        }
        if (why.empty()) {
          ++passed;
          continue;
        }
        if (IsEstimateSuite(stem) && why.find("7FC00000") == std::string::npos) {
          ++estimates;
          continue;
        }
        ++failed;
        ++failures_by_suite[stem];
        if (verbose || printed++ < 20)
          std::printf("FAIL %s: %s\n", name.c_str(), why.c_str());
      }
    }
  }

  std::printf("interpreter corpus: %llu cases, %llu passed, %llu failed, %llu unsupported\n",
              (unsigned long long)total, (unsigned long long)passed, (unsigned long long)failed,
              (unsigned long long)unsupported);
  std::printf("  skip.txt cases: %llu (%llu pass here); canary-skip cases: %llu (%llu fail)\n",
              (unsigned long long)skip_listed, (unsigned long long)skip_listed_passing,
              (unsigned long long)canary_listed, (unsigned long long)canary_failed);
  std::printf("  estimate cases with the exact value, not hardware's: %llu\n",
              (unsigned long long)estimates);
  for (const auto& [suite, n] : failures_by_suite)
    std::printf("  failed   %-28s %llu\n", suite.c_str(), (unsigned long long)n);
  if (verbose)
    for (const auto& [suite, n] : unsupported_by_suite)
      std::printf("  unsupported %-25s %llu\n", suite.c_str(), (unsigned long long)n);
  CHECK(total > 0);
  CHECK(failed == 0);
}
