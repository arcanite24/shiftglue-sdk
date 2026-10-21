#pragma once

// Region oracle support (codegen option oracle_regions; pinyon-shift #749).
// For every function of a listed region the generator emits two more copies
// with every store routed through the reference log (rex/ppc/reference.h):
// `orr_sub_X` with the reference options (every register in ctx, no
// folding, no fast paths) and `orc_sub_X` with the build's own options and
// specialisations, the candidate. Calls between region functions stay inside
// the same set of copies. A call anywhere else (an import, an indirect call,
// a function outside the region) is an opaque boundary: the copy records
// where it stopped and the whole context there, and returns to the host
// oracle through OracleSide::exit, so the callee never runs on a copy.
//
// The CRT functions the host runs natively (kernel/crt/memory.cpp: memcpy,
// memmove, memset) are not boundaries: they only move bytes and leave every
// register as it was, so the copies call `orr_rexcrt_<name>` and
// `orc_rexcrt_<name>`, which the host oracle defines with OracleCrtRun, and
// the interpreter runs the same function through its host_call hook.
//
// The host oracle runs each copy and the slow interpreter
// (rex/ppc/interpreter.h) from the same input, rolls their stores back, and
// compares the full context, the ordered stores and the exit path.

#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/reference.h>
#include <rex/system/xmemory.h>

namespace rex::ppc {

struct OracleSide {
  std::jmp_buf exit;
  bool halted = false;
  uint32_t target = 0;  // the boundary's callee (0 when unknown)
  uint32_t site = 0;    // the calling instruction (0 for an indirect call)
  PPCContext at;        // the context at the boundary
};

inline OracleSide*& OracleCurrentSide() {
  thread_local OracleSide* side = nullptr;
  return side;
}

// Called by a region copy in place of a call that leaves the region. Region
// copies have no destructors to run, so the jump is a plain unwind.
[[noreturn]] inline void OracleOpaque(const PPCContext& ctx, uint32_t target, uint32_t site) {
  OracleSide* side = OracleCurrentSide();
  if (!side)
    std::abort();  // a region copy ran outside the oracle
  side->halted = true;
  side->target = target;
  side->site = site;
  side->at = ctx;
  std::longjmp(side->exit, 1);
}

enum class OracleCrt : uint8_t { kNone, kMemcpy, kMemmove, kMemset };

inline OracleCrt OracleCrtByName(std::string_view name) {
  if (name == "memcpy") return OracleCrt::kMemcpy;
  if (name == "memmove") return OracleCrt::kMemmove;
  if (name == "memset") return OracleCrt::kMemset;
  return OracleCrt::kNone;
}

// The host function's effect, logged: r3 destination, r4 source (or the fill
// byte), r5 length; ascending 16-byte pieces, registers unchanged.
inline void OracleCrtRun(OracleCrt kind, PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t n = ctx.r5.u32;
  auto host = [base](uint32_t address) {
    return base + address + rex::memory::detail::PhysicalHostOffset(address);
  };
  std::vector<uint8_t> bytes(n);
  if (kind == OracleCrt::kMemset)
    std::memset(bytes.data(), int(uint8_t(ctx.r4.u32)), n);
  else if (n)
    std::memcpy(bytes.data(), host(ctx.r4.u32), n);  // memmove: read before writing
  for (uint32_t at = 0; at < n; at += 16) {
    const uint8_t size = uint8_t(n - at < 16 ? n - at : 16);
    RefStoreBytes(host(dst + at), dst + at, bytes.data() + at, size);
  }
}

}  // namespace rex::ppc

// Native regions (codegen option native_regions, #750): every region
// function is emitted again as an always-inline copy that takes the region's
// local context `ctx`, a stack copy whose address never escapes, so Clang
// keeps registers in SSA values across calls inside the region. `real` is the
// caller's context: a call that leaves the region publishes the local context
// to it, makes the call there and reloads afterwards. The generated
// `nr_sub_X` copies the context in, runs the region and copies it back.
#define REX_NR_FN(name)                                                             \
  static inline __attribute__((always_inline)) void name(                          \
      PPCContext& __restrict ctx, [[maybe_unused]] uint8_t* base,                  \
      [[maybe_unused]] PPCContext& __restrict real)

// The generated call is spelled with `ctx, base` so the register-locals sync
// passes publish the localized registers before it, as before a real call.
#define REX_ORACLE_OPAQUE(ctx, base, target, site) \
  ((void)(base), ::rex::ppc::OracleOpaque((ctx), uint32_t(target), uint32_t(site)))
