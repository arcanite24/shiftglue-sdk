#pragma once

// A slow PowerPC interpreter for the region oracle (pinyon-shift #749). It
// runs a bounded region of guest code instruction by instruction, on a
// PPCContext, with every store routed through the reference log
// (rex/ppc/reference.h) so the caller can compare and roll it back.
//
// It is written apart from the generator on purpose: its instruction
// semantics come from the PowerPC and VMX128 encodings, not from the
// builders in src/codegen, so a generator bug cannot pass both the
// recompiled code and this reference. tests/ppc/interp_cases.cpp runs it on
// the instruction test corpus, including the 167,636 hardware-recorded
// cases, to keep it honest.
//
// Scope: the integer, scalar FP and VMX/VMX128 forms bounded regions use.
// Anything else stops with Exit::kUnsupported and the instruction word, so a
// region that needs more fails loudly instead of being compared wrongly.
// FPSCR status bits, XER[OV], the reservation and FP record forms are not
// modelled. Reciprocal estimates return the correctly rounded value, as the
// recompiled code does (the hardware tables are not public).
//
// Register layout follows PPCContext: a vector register's guest byte i is
// host byte 15 - i, so guest word i is u32[3 - i].

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <rex/platform/fpscr.h>
#include <rex/ppc/context.h>
#include <rex/ppc/reference.h>
#include <rex/system/xmemory.h>

namespace rex::ppc::interp {

enum class Exit : uint8_t {
  kReturned,     // blr out of the entry function
  kBoundary,     // a call or branch to code outside the region
  kUnsupported,  // an instruction this interpreter does not implement
  kStepLimit,    // ran longer than Config::max_steps
  kFetchFailed,  // the instruction fetch callback had nothing at pc
};

inline const char* ExitName(Exit exit) {
  switch (exit) {
    case Exit::kReturned:
      return "returned";
    case Exit::kBoundary:
      return "boundary";
    case Exit::kUnsupported:
      return "unsupported";
    case Exit::kStepLimit:
      return "step_limit";
    case Exit::kFetchFailed:
      return "fetch_failed";
  }
  return "?";
}

struct Config {
  // Instruction words: true and the word in `*insn` when `address` holds code.
  bool (*fetch)(void* user, uint32_t address, uint32_t* insn) = nullptr;
  void* fetch_user = nullptr;
  // Whether a call (bl, bcl, bctrl) or a branch may enter `address`; a target
  // that is not inside ends the run with Exit::kBoundary. nullptr: anything.
  bool (*inside)(void* user, uint32_t address) = nullptr;
  void* inside_user = nullptr;
  // A function the host runs in place of guest code (the CRT's memcpy and
  // memset): for a target outside the region, true when it ran the function
  // on `ctx` and memory, and the run goes on as if the call returned.
  bool (*host_call)(void* user, PPCContext& ctx, uint8_t* base, uint32_t target) = nullptr;
  void* host_call_user = nullptr;
  // VMX non-Java mode: denormal inputs and results become signed zeros, as on
  // the console. The runtime keeps them in fixed FP mode (ppc_fixed_fp_mode).
  bool vmx_flush_denormals = true;
  uint64_t max_steps = 50'000'000;
};

struct Result {
  Exit exit = Exit::kReturned;
  uint32_t pc = 0;               // the instruction that stopped the run
  uint32_t insn = 0;             // its word (kUnsupported)
  uint32_t target = 0;           // the boundary's target (kBoundary)
  uint64_t steps = 0;
  uint32_t return_pc = 0;        // the blr that returned (kReturned)
};

namespace detail {

inline uint8_t* Host(uint8_t* base, uint32_t address) {
  return base + address + rex::memory::detail::PhysicalHostOffset(address);
}

template <typename T>
inline T LoadBE(uint8_t* base, uint32_t address) {
  T value;
  std::memcpy(&value, Host(base, address), sizeof(T));
  if constexpr (sizeof(T) == 2)
    value = T(__builtin_bswap16(uint16_t(value)));
  else if constexpr (sizeof(T) == 4)
    value = T(__builtin_bswap32(uint32_t(value)));
  else if constexpr (sizeof(T) == 8)
    value = T(__builtin_bswap64(uint64_t(value)));
  return value;
}

// Every store is logged: the oracle compares the log and rolls it back.
inline void StoreBytes(uint8_t* base, uint32_t address, const uint8_t* bytes, uint8_t size) {
  RefStoreBytes(Host(base, address), address, bytes, size);
}

template <typename T>
inline void StoreBE(uint8_t* base, uint32_t address, T value) {
  uint8_t bytes[sizeof(T)];
  for (size_t i = 0; i < sizeof(T); ++i)
    bytes[i] = uint8_t(uint64_t(value) >> (8 * (sizeof(T) - 1 - i)));
  StoreBytes(base, address, bytes, sizeof(T));
}

inline uint64_t Mask64(uint32_t mb, uint32_t me) {
  // Bits mb..me in PowerPC numbering (0 = most significant), wrapping when
  // mb > me.
  const uint64_t from = ~0ull >> mb;
  const uint64_t to = ~0ull << (63 - me);
  return mb <= me ? (from & to) : (from | to);
}

inline uint64_t Rotl64(uint64_t value, uint32_t n) {
  n &= 63;
  return n ? (value << n) | (value >> (64 - n)) : value;
}

inline int64_t Exts(uint64_t value, int bits) {
  const uint64_t m = 1ull << (bits - 1);
  value &= (bits == 64) ? ~0ull : ((1ull << bits) - 1);
  return int64_t((value ^ m) - m);
}

// --- Floating point -------------------------------------------------------

inline double BitsToDouble(uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}
inline uint64_t DoubleToBits(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  return bits;
}
inline float BitsToFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
inline uint32_t FloatToBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

// lfs: single to double without rounding or quieting (a signaling NaN stays
// signaling; a host conversion would quiet it).
inline uint64_t SingleToDoubleBits(uint32_t s) {
  const uint64_t sign = uint64_t(s >> 31) << 63;
  const uint32_t exp = (s >> 23) & 0xFF;
  const uint64_t man = s & 0x7FFFFF;
  if (exp == 0xFF)
    return sign | 0x7FF0000000000000ull | (man << 29);
  if (exp == 0) {
    if (man == 0)
      return sign;
    // A denormal single is a normal double: exact in double arithmetic.
    const double d = std::ldexp(double(man), -149);
    return sign | DoubleToBits(d);
  }
  return sign | (uint64_t(exp - 127 + 1023) << 52) | (man << 29);
}

// stfs: the "store single" conversion. In single range the exponent is
// rebiased and the fraction truncated (the value is expected to be single
// already); below it the value is denormalized; NaN keeps its high fraction.
inline uint32_t DoubleToSingleBits(uint64_t d) {
  const uint32_t sign = uint32_t(d >> 63) << 31;
  const int exp = int((d >> 52) & 0x7FF);
  const uint64_t frac = d & 0xFFFFFFFFFFFFFull;
  if (exp > 896 || (d & 0x7FFFFFFFFFFFFFFFull) == 0) {
    // Normal single range, zero, infinity and NaN: bits 0-1 and 5-34.
    return uint32_t(((d >> 32) & 0xC0000000u) | ((d >> 29) & 0x3FFFFFFFu));
  }
  if (exp >= 874) {
    // Denormalize: the implicit one and the fraction shifted right.
    const uint64_t man = (1ull << 52) | frac;
    const int shift = 29 + (897 - exp);
    return sign | uint32_t(man >> shift);
  }
  // Below the denormal range the result is undefined; keep the sign.
  return sign;
}

inline double RoundToSingle(double value) { return double(float(value)); }

// NaN results: the first NaN operand (frA, then frB, then frC) quieted, its
// sign kept even by the negating forms; an invalid operation with no NaN
// operand gives the default QNaN, which is positive (x86's is negative).
inline bool PickNaN(double a, double b, double c, int mask, double* out) {
  const double ops[3] = {a, b, c};
  for (int i = 0; i < 3; ++i) {
    if ((mask >> i & 1) && std::isnan(ops[i])) {
      *out = BitsToDouble(DoubleToBits(ops[i]) | 0x0008000000000000ull);
      return true;
    }
  }
  return false;
}
inline double DefaultNaN() { return BitsToDouble(0x7FF8000000000000ull); }
inline double FixNaN(double r) { return std::isnan(r) ? DefaultNaN() : r; }

inline bool IsSingle(double value) {
  return std::isnan(value) || double(float(value)) == value;
}

// fmadds and its family: one rounding to single when the operands are
// singles (fmaf is then exact); a double fma rounded to single otherwise.
inline double FmaSingle(double a, double c, double b) {
  if (IsSingle(a) && IsSingle(c) && IsSingle(b))
    return double(std::fmaf(float(a), float(c), float(b)));
  return RoundToSingle(std::fma(a, c, b));
}

inline uint64_t ConvertToInt64(double value, bool truncate) {
  if (std::isnan(value))
    return 0x8000000000000000ull;
  const double r = truncate ? std::trunc(value) : std::nearbyint(value);
  if (r >= 9223372036854775808.0)
    return 0x7FFFFFFFFFFFFFFFull;
  if (r < -9223372036854775808.0)
    return 0x8000000000000000ull;
  return uint64_t(int64_t(r));
}

inline uint64_t ConvertToInt32(double value, bool truncate) {
  if (std::isnan(value))
    return 0xFFFFFFFF80000000ull;
  const double r = truncate ? std::trunc(value) : std::nearbyint(value);
  if (r >= 2147483648.0)
    return 0x7FFFFFFFull;
  if (r < -2147483648.0)
    return 0xFFFFFFFF80000000ull;
  return uint64_t(int64_t(int32_t(r)));
}

// --- VMX ------------------------------------------------------------------

inline uint32_t GuestWord(const PPCVRegister& v, int i) { return v.u32[3 - i]; }
inline void SetGuestWord(PPCVRegister& v, int i, uint32_t value) { v.u32[3 - i] = value; }

inline float Flush(float value, bool flush) {
  if (flush && std::fpclassify(value) == FP_SUBNORMAL)
    return std::copysign(0.0f, value);
  return value;
}

inline uint32_t VmxNaN() { return 0x7FC00000u; }

inline bool IsNaN32(uint32_t bits) {
  return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu);
}

// A lane with a NaN operand returns the first one in `order` quieted.
inline bool VmxPickNaN(const uint32_t* lanes, const int* order, int n, uint32_t* out) {
  for (int i = 0; i < n; ++i) {
    if (IsNaN32(lanes[order[i]])) {
      *out = lanes[order[i]] | 0x00400000u;
      return true;
    }
  }
  return false;
}

template <typename Op>
inline void VmxFloat2(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b, bool flush,
                      Op op) {
  PPCVRegister r;
  static constexpr int kOrder[2] = {0, 1};
  for (int i = 0; i < 4; ++i) {
    const uint32_t lanes[2] = {GuestWord(a, i), GuestWord(b, i)};
    uint32_t nan;
    if (VmxPickNaN(lanes, kOrder, 2, &nan)) {
      SetGuestWord(r, i, nan);
      continue;
    }
    const float x = Flush(BitsToFloat(lanes[0]), flush);
    const float y = Flush(BitsToFloat(lanes[1]), flush);
    const float z = Flush(op(x, y), flush);
    SetGuestWord(r, i, std::isnan(z) ? VmxNaN() : FloatToBits(z));
  }
  d = r;
}

// `order`: which of the three operands a NaN result is taken from first.
template <typename Op>
inline void VmxFloat3(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b,
                      const PPCVRegister& c, bool flush, const int* order, Op op) {
  PPCVRegister r;
  for (int i = 0; i < 4; ++i) {
    const uint32_t lanes[3] = {GuestWord(a, i), GuestWord(b, i), GuestWord(c, i)};
    uint32_t nan;
    if (VmxPickNaN(lanes, order, 3, &nan)) {
      SetGuestWord(r, i, nan);
      continue;
    }
    const float x = Flush(BitsToFloat(lanes[0]), flush);
    const float y = Flush(BitsToFloat(lanes[1]), flush);
    const float z = Flush(BitsToFloat(lanes[2]), flush);
    const float w = Flush(op(x, y, z), flush);
    SetGuestWord(r, i, std::isnan(w) ? VmxNaN() : FloatToBits(w));
  }
  d = r;
}

// Host FP environment for the run: round to nearest, no flush-to-zero, no
// denormals-are-zero, exceptions masked. Restored on exit.
class HostFpScope {
 public:
  HostFpScope() {
    using P = rex::platform::FPSCRPlatform;
    saved_ = P::getcsr();
    uint32_t csr = saved_ & ~uint32_t(P::RoundMaskVal | P::FlushMask);
    P::InitHostExceptions(csr);
    P::setcsr(csr);
  }
  ~HostFpScope() { rex::platform::FPSCRPlatform::setcsr(saved_); }
  HostFpScope(const HostFpScope&) = delete;
  HostFpScope& operator=(const HostFpScope&) = delete;

 private:
  uint32_t saved_;
};

// NaN priority for VmxFloat3's (x, y, z): in argument order, or vA, vB, vC
// for the VA forms called as (vA, vC, vB).
inline constexpr int kNanInOrder[3] = {0, 1, 2};
inline constexpr int kNanAcb[3] = {0, 2, 1};

class Machine {
 public:
  Machine(PPCContext& ctx, uint8_t* base, const Config& config)
      : ctx_(ctx), base_(base), config_(config) {}

  Result Run(uint32_t entry) {
    HostFpScope fp;
    Result result;
    uint32_t pc = entry;
    uint32_t depth = 0;
    for (;;) {
      if (result.steps++ >= config_.max_steps) {
        result.exit = Exit::kStepLimit;
        result.pc = pc;
        return result;
      }
      uint32_t insn = 0;
      if (!config_.fetch(config_.fetch_user, pc, &insn)) {
        result.exit = Exit::kFetchFailed;
        result.pc = pc;
        return result;
      }
      Flow flow;
      if (!Step(pc, insn, flow)) {
        result.exit = Exit::kUnsupported;
        result.pc = pc;
        result.insn = insn;
        return result;
      }
      if (!flow.taken) {
        pc += 4;
        continue;
      }
      if (flow.kind == Flow::kReturn) {
        if (depth == 0) {
          result.exit = Exit::kReturned;
          result.pc = pc;
          result.return_pc = pc;
          return result;
        }
        --depth;
        pc = flow.target;
        continue;
      }
      if (flow.kind != Flow::kLocal && !Inside(flow.target) && config_.host_call &&
          config_.host_call(config_.host_call_user, ctx_, base_, flow.target)) {
        if (flow.kind == Flow::kCall) {
          pc += 4;
          continue;
        }
        // A tail branch: the host function returned to our caller.
        if (depth == 0) {
          result.exit = Exit::kReturned;
          result.pc = pc;
          result.return_pc = pc;
          return result;
        }
        --depth;
        pc = uint32_t(ctx_.lr);
        continue;
      }
      if (flow.kind != Flow::kLocal && !Inside(flow.target)) {
        // The call's link register is set (as the hardware would); the
        // callee has not run.
        result.exit = Exit::kBoundary;
        result.pc = pc;
        result.target = flow.target;
        return result;
      }
      if (flow.kind == Flow::kCall)
        ++depth;
      pc = flow.target;
    }
  }

 private:
  struct Flow {
    enum Kind : uint8_t { kLocal, kCall, kTail, kReturn } kind = kLocal;
    bool taken = false;
    uint32_t target = 0;
  };

  bool Inside(uint32_t address) const {
    return !config_.inside || config_.inside(config_.inside_user, address);
  }

  PPCRegister& R(uint32_t n) { return *Gpr(n); }
  PPCRegister* Gpr(uint32_t n) {
    // PPCContext stores r3 first; map by name.
    switch (n) {
      case 0: return &ctx_.r0;
      case 1: return &ctx_.r1;
      case 2: return &ctx_.r2;
      case 3: return &ctx_.r3;
      default: return &ctx_.r4 + (n - 4);
    }
  }
  PPCRegister& F(uint32_t n) { return (&ctx_.f0)[n]; }
  PPCVRegister& V(uint32_t n) { return (&ctx_.v0)[n]; }
  PPCCRRegister& Cr(uint32_t n) { return (&ctx_.cr0)[n]; }

  uint64_t RA0(uint32_t ra) { return ra ? R(ra).u64 : 0; }

  // XER[CA] of a + b + c, out of the low word.
  static bool Carry32(uint64_t a, uint64_t b, uint64_t c) {
    return (uint64_t(uint32_t(a)) + uint32_t(b) + c) >> 32;
  }

  bool CrBit(uint32_t bi) {
    const PPCCRRegister& f = Cr(bi >> 2);
    switch (bi & 3) {
      case 0: return f.lt;
      case 1: return f.gt;
      case 2: return f.eq;
      default: return f.so;
    }
  }

  void SetCr0(uint64_t value) {
    // The console's record forms compare the low word, as its carries do
    // (addc. of 0x7FFFFFFFFFFFFFFF and 2 sets CA and CR0[GT]; tests/ppc).
    const int32_t v = int32_t(uint32_t(value));
    Cr(0).lt = v < 0;
    Cr(0).gt = v > 0;
    Cr(0).eq = v == 0;
    Cr(0).so = ctx_.xer.so;
  }

  bool Branch(uint32_t pc, uint32_t bo, uint32_t bi, uint32_t target, bool lk, Flow& flow,
              Flow::Kind kind) {
    if (!(bo & 4)) {
      ctx_.ctr.u64 -= 1;
    }
    const bool ctr_ok = (bo & 4) || ((ctx_.ctr.u32 != 0) != bool(bo & 2));
    const bool cond_ok = (bo & 16) || (CrBit(bi) == bool(bo & 8));
    if (lk)
      ctx_.lr = pc + 4;
    if (ctr_ok && cond_ok) {
      flow.taken = true;
      flow.target = target;
      flow.kind = kind;
    }
    return true;
  }

  // Loads and stores -----------------------------------------------------

  void LoadVector(uint32_t vd, uint32_t ea) {
    PPCVRegister v;
    for (int i = 0; i < 16; ++i)
      v.u8[15 - i] = LoadBE<uint8_t>(base_, ea + i);
    V(vd) = v;
  }
  void StoreVector(uint32_t vs, uint32_t ea) {
    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i)
      bytes[i] = V(vs).u8[15 - i];
    StoreBytes(base_, ea, bytes, 16);
  }
  void LoadVectorLeft(uint32_t vd, uint32_t ea) {
    PPCVRegister v{};
    const uint32_t n = 16 - (ea & 15);
    for (uint32_t i = 0; i < n; ++i)
      v.u8[15 - i] = LoadBE<uint8_t>(base_, ea + i);
    V(vd) = v;
  }
  void LoadVectorRight(uint32_t vd, uint32_t ea) {
    PPCVRegister v{};
    const uint32_t n = ea & 15;
    const uint32_t aligned = ea & ~15u;
    for (uint32_t i = 0; i < n; ++i)
      v.u8[15 - (16 - n + i)] = LoadBE<uint8_t>(base_, aligned + i);
    V(vd) = v;
  }
  void StoreVectorLeft(uint32_t vs, uint32_t ea) {
    const uint32_t n = 16 - (ea & 15);
    uint8_t bytes[16];
    for (uint32_t i = 0; i < n; ++i)
      bytes[i] = V(vs).u8[15 - i];
    StoreBytes(base_, ea, bytes, uint8_t(n));
  }
  void StoreVectorRight(uint32_t vs, uint32_t ea) {
    const uint32_t n = ea & 15;
    if (!n)
      return;
    uint8_t bytes[16];
    for (uint32_t i = 0; i < n; ++i)
      bytes[i] = V(vs).u8[15 - (16 - n + i)];
    StoreBytes(base_, ea & ~15u, bytes, uint8_t(n));
  }

  // One instruction. False: not implemented.
  bool Step(uint32_t pc, uint32_t insn, Flow& flow) {
    const uint32_t op = insn >> 26;
    const uint32_t rd = (insn >> 21) & 31;
    const uint32_t ra = (insn >> 16) & 31;
    const uint32_t rb = (insn >> 11) & 31;
    const uint32_t rc = (insn >> 6) & 31;
    const bool record = insn & 1;
    const int64_t simm = int16_t(insn & 0xFFFF);
    const uint64_t uimm = insn & 0xFFFF;

    switch (op) {
      case 4:
        return StepVmx(insn);
      case 5:
        return StepVmx128Arith(insn);
      case 6:
        return StepVmx128Unary(insn);
      case 7:  // mulli
        R(rd).s64 = int64_t(uint64_t(R(ra).s64) * uint64_t(simm));
        return true;
      case 8: {  // subfic
        const uint64_t a = R(ra).u64;
        R(rd).u64 = uint64_t(simm) - a;
        ctx_.xer.ca = uint32_t(simm) >= uint32_t(a);  // no borrow
        return true;
      }
      case 10:  // cmpli
        return CompareLogical(rd, ra, uimm);
      case 11:  // cmpi
        return CompareArith(rd, ra, simm);
      case 12:
      case 13: {  // addic, addic.
        const uint64_t a = R(ra).u64;
        const uint64_t r = a + uint64_t(simm);
        ctx_.xer.ca = Carry32(a, uint64_t(simm), 0);
        R(rd).u64 = r;
        if (op == 13)
          SetCr0(r);
        return true;
      }
      case 14:  // addi
        R(rd).u64 = RA0(ra) + uint64_t(simm);
        return true;
      case 15:  // addis
        R(rd).u64 = RA0(ra) + uint64_t(simm << 16);
        return true;
      case 16: {  // bc
        const uint32_t bd = uint32_t(Exts(insn & 0xFFFC, 16));
        const uint32_t target = (insn & 2) ? bd : pc + bd;
        return Branch(pc, rd, ra, target, insn & 1, flow,
                      (insn & 1) ? Flow::kCall : Flow::kLocal);
      }
      case 18: {  // b
        const uint32_t li = uint32_t(Exts(insn & 0x03FFFFFC, 26));
        const uint32_t target = (insn & 2) ? li : pc + li;
        if (insn & 1)
          ctx_.lr = pc + 4;
        flow.taken = true;
        flow.target = target;
        flow.kind = (insn & 1) ? Flow::kCall : Flow::kTail;
        return true;
      }
      case 19:
        return Step19(pc, insn, flow);
      case 20: {  // rlwimi
        const uint32_t sh = rb, mb = rc, me = (insn >> 1) & 31;
        const uint64_t low = R(rd).u32;
        const uint64_t rot = Rotl64(low | (low << 32), sh);
        const uint64_t m = Mask64(mb + 32, me + 32);
        R(ra).u64 = (rot & m) | (R(ra).u64 & ~m);
        if (record) SetCr0(R(ra).u64);
        return true;
      }
      case 21: {  // rlwinm
        const uint32_t sh = rb, mb = rc, me = (insn >> 1) & 31;
        const uint64_t low = R(rd).u32;
        R(ra).u64 = Rotl64(low | (low << 32), sh) & Mask64(mb + 32, me + 32);
        if (record) SetCr0(R(ra).u64);
        return true;
      }
      case 23: {  // rlwnm
        const uint32_t mb = rc, me = (insn >> 1) & 31;
        const uint64_t low = R(rd).u32;
        R(ra).u64 = Rotl64(low | (low << 32), R(rb).u32 & 31) & Mask64(mb + 32, me + 32);
        if (record) SetCr0(R(ra).u64);
        return true;
      }
      case 24:  // ori
        R(ra).u64 = R(rd).u64 | uimm;
        return true;
      case 25:  // oris
        R(ra).u64 = R(rd).u64 | (uimm << 16);
        return true;
      case 26:  // xori
        R(ra).u64 = R(rd).u64 ^ uimm;
        return true;
      case 27:  // xoris
        R(ra).u64 = R(rd).u64 ^ (uimm << 16);
        return true;
      case 28:  // andi.
        R(ra).u64 = R(rd).u64 & uimm;
        SetCr0(R(ra).u64);
        return true;
      case 29:  // andis.
        R(ra).u64 = R(rd).u64 & (uimm << 16);
        SetCr0(R(ra).u64);
        return true;
      case 30:
        return Step30(insn);
      case 31:
        return Step31(insn);
      case 32: case 33: {  // lwz, lwzu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        R(rd).u64 = LoadBE<uint32_t>(base_, ea);
        if (op == 33) R(ra).u64 = ea;
        return true;
      }
      case 34: case 35: {  // lbz, lbzu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        R(rd).u64 = LoadBE<uint8_t>(base_, ea);
        if (op == 35) R(ra).u64 = ea;
        return true;
      }
      case 36: case 37: {  // stw, stwu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        StoreBE<uint32_t>(base_, ea, R(rd).u32);
        if (op == 37) R(ra).u64 = ea;
        return true;
      }
      case 38: case 39: {  // stb, stbu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        StoreBE<uint8_t>(base_, ea, R(rd).u8);
        if (op == 39) R(ra).u64 = ea;
        return true;
      }
      case 40: case 41: {  // lhz, lhzu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        R(rd).u64 = LoadBE<uint16_t>(base_, ea);
        if (op == 41) R(ra).u64 = ea;
        return true;
      }
      case 42: case 43: {  // lha, lhau
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        R(rd).s64 = int16_t(LoadBE<uint16_t>(base_, ea));
        if (op == 43) R(ra).u64 = ea;
        return true;
      }
      case 44: case 45: {  // sth, sthu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        StoreBE<uint16_t>(base_, ea, R(rd).u16);
        if (op == 45) R(ra).u64 = ea;
        return true;
      }
      case 48: case 49: {  // lfs, lfsu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        F(rd).u64 = SingleToDoubleBits(LoadBE<uint32_t>(base_, ea));
        if (op == 49) R(ra).u64 = ea;
        return true;
      }
      case 50: case 51: {  // lfd, lfdu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        F(rd).u64 = LoadBE<uint64_t>(base_, ea);
        if (op == 51) R(ra).u64 = ea;
        return true;
      }
      case 52: case 53: {  // stfs, stfsu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        StoreBE<uint32_t>(base_, ea, DoubleToSingleBits(F(rd).u64));
        if (op == 53) R(ra).u64 = ea;
        return true;
      }
      case 54: case 55: {  // stfd, stfdu
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(simm));
        StoreBE<uint64_t>(base_, ea, F(rd).u64);
        if (op == 55) R(ra).u64 = ea;
        return true;
      }
      case 58: {  // ld, ldu, lwa
        const int64_t ds = Exts(insn & 0xFFFC, 16);
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(ds));
        switch (insn & 3) {
          case 0: R(rd).u64 = LoadBE<uint64_t>(base_, ea); return true;
          case 1: R(rd).u64 = LoadBE<uint64_t>(base_, ea); R(ra).u64 = ea; return true;
          case 2: R(rd).s64 = int32_t(LoadBE<uint32_t>(base_, ea)); return true;
        }
        return false;
      }
      case 59:
        return StepFpSingle(insn);
      case 62: {  // std, stdu
        const int64_t ds = Exts(insn & 0xFFFC, 16);
        const uint32_t ea = uint32_t(RA0(ra) + uint64_t(ds));
        switch (insn & 3) {
          case 0: StoreBE<uint64_t>(base_, ea, R(rd).u64); return true;
          case 1: StoreBE<uint64_t>(base_, ea, R(rd).u64); R(ra).u64 = ea; return true;
        }
        return false;
      }
      case 63:
        return StepFpDouble(insn);
    }
    return false;
  }

  bool CompareArith(uint32_t crfd_l, uint32_t ra, int64_t b) {
    const uint32_t crf = crfd_l >> 2;
    const bool l = crfd_l & 1;
    const int64_t a = l ? R(ra).s64 : int64_t(R(ra).s32);
    const int64_t bb = l ? b : int64_t(int32_t(b));
    Cr(crf).lt = a < bb;
    Cr(crf).gt = a > bb;
    Cr(crf).eq = a == bb;
    Cr(crf).so = ctx_.xer.so;
    return true;
  }
  bool CompareLogical(uint32_t crfd_l, uint32_t ra, uint64_t b) {
    const uint32_t crf = crfd_l >> 2;
    const bool l = crfd_l & 1;
    const uint64_t a = l ? R(ra).u64 : uint64_t(R(ra).u32);
    const uint64_t bb = l ? b : uint64_t(uint32_t(b));
    Cr(crf).lt = a < bb;
    Cr(crf).gt = a > bb;
    Cr(crf).eq = a == bb;
    Cr(crf).so = ctx_.xer.so;
    return true;
  }

  bool Step19(uint32_t pc, uint32_t insn, Flow& flow) {
    const uint32_t xo = (insn >> 1) & 0x3FF;
    const uint32_t bo = (insn >> 21) & 31, bi = (insn >> 16) & 31;
    const bool lk = insn & 1;
    switch (xo) {
      case 16: {  // bclr
        const uint32_t target = uint32_t(ctx_.lr) & ~3u;
        return Branch(pc, bo, bi, target, lk, flow, lk ? Flow::kCall : Flow::kReturn);
      }
      case 528: {  // bcctr
        if (!(bo & 4))
          return false;  // invalid form
        const uint32_t target = ctx_.ctr.u32 & ~3u;
        return Branch(pc, bo, bi, target, lk, flow, lk ? Flow::kCall : Flow::kTail);
      }
      case 150:  // isync
        return true;
      case 0: {  // mcrf
        Cr((insn >> 23) & 7) = Cr((insn >> 18) & 7);
        return true;
      }
      case 257: case 129: case 289: case 225: case 33: case 449: case 417: case 193: {
        // crand, crandc, creqv, crnand, crnor, cror, crorc, crxor
        const bool a = CrBit((insn >> 16) & 31), b = CrBit((insn >> 11) & 31);
        bool r = false;
        switch (xo) {
          case 257: r = a && b; break;
          case 129: r = a && !b; break;
          case 289: r = a == b; break;
          case 225: r = !(a && b); break;
          case 33: r = !(a || b); break;
          case 449: r = a || b; break;
          case 417: r = a || !b; break;
          case 193: r = a != b; break;
        }
        const uint32_t bt = (insn >> 21) & 31;
        PPCCRRegister& f = Cr(bt >> 2);
        switch (bt & 3) {
          case 0: f.lt = r; break;
          case 1: f.gt = r; break;
          case 2: f.eq = r; break;
          default: f.so = r; break;
        }
        return true;
      }
    }
    return false;
  }

  bool Step30(uint32_t insn) {
    const uint32_t rs = (insn >> 21) & 31, ra = (insn >> 16) & 31;
    const uint32_t sh = ((insn >> 11) & 31) | ((insn & 2) << 4);
    const uint32_t raw = (insn >> 5) & 0x3F;
    const uint32_t mbe = ((raw & 1) << 5) | (raw >> 1);
    const bool record = insn & 1;
    const uint64_t rot = Rotl64(R(rs).u64, sh);
    switch ((insn >> 2) & 7) {
      case 0:  // rldicl
        R(ra).u64 = rot & Mask64(mbe, 63);
        break;
      case 1:  // rldicr
        R(ra).u64 = rot & Mask64(0, mbe);
        break;
      case 2:  // rldic
        R(ra).u64 = rot & Mask64(mbe, 63 - sh);
        break;
      case 3: {  // rldimi
        const uint64_t m = Mask64(mbe, 63 - sh);
        R(ra).u64 = (rot & m) | (R(ra).u64 & ~m);
        break;
      }
      default:
        return false;
    }
    if (record) SetCr0(R(ra).u64);
    return true;
  }

  bool Step31(uint32_t insn) {
    const uint32_t rd = (insn >> 21) & 31, ra = (insn >> 16) & 31, rb = (insn >> 11) & 31;
    const bool record = insn & 1;
    const uint32_t xo = (insn >> 1) & 0x3FF;
    const uint32_t xo9 = (insn >> 1) & 0x1FF;
    const bool oe = insn & 0x400;
    auto ea_x = [&] { return uint32_t(RA0(ra) + R(rb).u64); };
    auto ea_ux = [&] { return uint32_t(R(ra).u64 + R(rb).u64); };

    // XO-form arithmetic (9-bit extended opcode, OE not modelled).
    switch (xo9) {
      case 266:  // add
        if (oe) return false;
        R(rd).u64 = R(ra).u64 + R(rb).u64;
        if (record) SetCr0(R(rd).u64);
        return true;
      case 40:  // subf
        if (oe) return false;
        R(rd).u64 = R(rb).u64 - R(ra).u64;
        if (record) SetCr0(R(rd).u64);
        return true;
      case 104:  // neg
        if (oe) return false;
        R(rd).u64 = ~R(ra).u64 + 1;
        if (record) SetCr0(R(rd).u64);
        return true;
      case 10: {  // addc
        if (oe) return false;
        const uint64_t a = R(ra).u64, r = a + R(rb).u64;
        ctx_.xer.ca = Carry32(a, R(rb).u64, 0);
        R(rd).u64 = r;
        if (record) SetCr0(r);
        return true;
      }
      case 8: {  // subfc
        if (oe) return false;
        const uint64_t a = R(ra).u64, b = R(rb).u64;
        ctx_.xer.ca = uint32_t(b) >= uint32_t(a);
        R(rd).u64 = b - a;
        if (record) SetCr0(R(rd).u64);
        return true;
      }
      case 138: {  // adde
        if (oe) return false;
        const uint64_t a = R(ra).u64, b = R(rb).u64, c = ctx_.xer.ca;
        const uint64_t r = a + b + c;
        ctx_.xer.ca = Carry32(a, b, c);
        R(rd).u64 = r;
        if (record) SetCr0(r);
        return true;
      }
      case 235:  // mullw
        if (oe) return false;
        R(rd).s64 = int64_t(R(ra).s32) * int64_t(R(rb).s32);
        if (record) SetCr0(R(rd).u64);
        return true;
      case 233:  // mulld
        if (oe) return false;
        R(rd).u64 = R(ra).u64 * R(rb).u64;
        if (record) SetCr0(R(rd).u64);
        return true;
      case 75:  // mulhw
        R(rd).s64 = (int64_t(R(ra).s32) * int64_t(R(rb).s32)) >> 32;
        if (record) SetCr0(R(rd).u64);
        return true;
      case 11:  // mulhwu
        R(rd).u64 = (uint64_t(R(ra).u32) * uint64_t(R(rb).u32)) >> 32;
        if (record) SetCr0(R(rd).u64);
        return true;
      case 491: {  // divw
        if (oe) return false;
        const int32_t a = R(ra).s32, b = R(rb).s32;
        if (b == 0 || (a == INT32_MIN && b == -1))
          return false;  // undefined result: let the oracle say so
        R(rd).u64 = uint32_t(a / b);
        if (record) SetCr0(R(rd).u64);
        return true;
      }
      case 459: {  // divwu
        if (oe) return false;
        const uint32_t a = R(ra).u32, b = R(rb).u32;
        if (b == 0)
          return false;
        R(rd).u64 = a / b;
        if (record) SetCr0(R(rd).u64);
        return true;
      }
    }

    switch (xo) {
      case 0:  // cmp
        return CompareArith(rd, ra, R(rb).s64);
      case 32:  // cmpl
        return CompareLogical(rd, ra, R(rb).u64);
      case 28: R(ra).u64 = R(rd).u64 & R(rb).u64; break;          // and
      case 60: R(ra).u64 = R(rd).u64 & ~R(rb).u64; break;         // andc
      case 444: R(ra).u64 = R(rd).u64 | R(rb).u64; break;         // or
      case 412: R(ra).u64 = R(rd).u64 | ~R(rb).u64; break;        // orc
      case 316: R(ra).u64 = R(rd).u64 ^ R(rb).u64; break;         // xor
      case 124: R(ra).u64 = ~(R(rd).u64 | R(rb).u64); break;      // nor
      case 476: R(ra).u64 = ~(R(rd).u64 & R(rb).u64); break;      // nand
      case 284: R(ra).u64 = ~(R(rd).u64 ^ R(rb).u64); break;      // eqv
      case 954: R(ra).s64 = int8_t(R(rd).u8); break;              // extsb
      case 922: R(ra).s64 = int16_t(R(rd).u16); break;            // extsh
      case 986: R(ra).s64 = int32_t(R(rd).u32); break;            // extsw
      case 26:  // cntlzw
        R(ra).u64 = R(rd).u32 ? uint64_t(__builtin_clz(R(rd).u32)) : 32;
        break;
      case 58:  // cntlzd
        R(ra).u64 = R(rd).u64 ? uint64_t(__builtin_clzll(R(rd).u64)) : 64;
        break;
      case 24: {  // slw
        const uint32_t n = R(rb).u32 & 63;
        R(ra).u64 = n > 31 ? 0 : uint32_t(R(rd).u32 << n);
        break;
      }
      case 536: {  // srw
        const uint32_t n = R(rb).u32 & 63;
        R(ra).u64 = n > 31 ? 0 : uint32_t(R(rd).u32 >> n);
        break;
      }
      case 792: {  // sraw
        const uint32_t n = R(rb).u32 & 63;
        const int32_t s = R(rd).s32;
        if (n > 31) {
          R(ra).s64 = s < 0 ? -1 : 0;
          ctx_.xer.ca = s < 0;
        } else {
          R(ra).s64 = s >> n;
          ctx_.xer.ca = s < 0 && n && (uint32_t(s) & ((1u << n) - 1));
        }
        break;
      }
      case 824: {  // srawi
        const uint32_t n = rb;
        const int32_t s = R(rd).s32;
        R(ra).s64 = s >> n;
        ctx_.xer.ca = s < 0 && n && (uint32_t(s) & ((1u << n) - 1));
        break;
      }
      case 27: {  // sld
        const uint32_t n = R(rb).u32 & 127;
        R(ra).u64 = n > 63 ? 0 : R(rd).u64 << n;
        break;
      }
      case 539: {  // srd
        const uint32_t n = R(rb).u32 & 127;
        R(ra).u64 = n > 63 ? 0 : R(rd).u64 >> n;
        break;
      }
      case 794: {  // srad
        const uint32_t n = R(rb).u32 & 127;
        const int64_t s = R(rd).s64;
        if (n > 63) {
          R(ra).s64 = s < 0 ? -1 : 0;
          ctx_.xer.ca = s < 0;
        } else {
          R(ra).s64 = s >> n;
          ctx_.xer.ca = s < 0 && n && (uint64_t(s) & ((1ull << n) - 1));
        }
        break;
      }
      case 826: case 827: {  // sradi (XS form: xo 413, sh[5] in the low bit)
        const uint32_t n = rb | ((insn & 2) << 4);
        const int64_t s = R(rd).s64;
        R(ra).s64 = s >> n;
        ctx_.xer.ca = s < 0 && n && (uint64_t(s) & ((1ull << n) - 1));
        break;
      }
      // Loads and stores: no record forms.
      case 23: R(rd).u64 = LoadBE<uint32_t>(base_, ea_x()); return true;                // lwzx
      case 55: { const uint32_t ea = ea_ux(); R(rd).u64 = LoadBE<uint32_t>(base_, ea); R(ra).u64 = ea; return true; }
      case 87: R(rd).u64 = LoadBE<uint8_t>(base_, ea_x()); return true;                 // lbzx
      case 279: R(rd).u64 = LoadBE<uint16_t>(base_, ea_x()); return true;               // lhzx
      case 343: R(rd).s64 = int16_t(LoadBE<uint16_t>(base_, ea_x())); return true;      // lhax
      case 341: R(rd).s64 = int32_t(LoadBE<uint32_t>(base_, ea_x())); return true;      // lwax
      case 21: R(rd).u64 = LoadBE<uint64_t>(base_, ea_x()); return true;                // ldx
      case 53: { const uint32_t ea = ea_ux(); R(rd).u64 = LoadBE<uint64_t>(base_, ea); R(ra).u64 = ea; return true; }
      case 534: R(rd).u64 = __builtin_bswap32(LoadBE<uint32_t>(base_, ea_x())); return true;  // lwbrx
      case 151: StoreBE<uint32_t>(base_, ea_x(), R(rd).u32); return true;               // stwx
      case 183: { const uint32_t ea = ea_ux(); StoreBE<uint32_t>(base_, ea, R(rd).u32); R(ra).u64 = ea; return true; }
      case 215: StoreBE<uint8_t>(base_, ea_x(), R(rd).u8); return true;                 // stbx
      case 407: StoreBE<uint16_t>(base_, ea_x(), R(rd).u16); return true;               // sthx
      case 149: StoreBE<uint64_t>(base_, ea_x(), R(rd).u64); return true;               // stdx
      case 181: { const uint32_t ea = ea_ux(); StoreBE<uint64_t>(base_, ea, R(rd).u64); R(ra).u64 = ea; return true; }
      case 662: StoreBE<uint32_t>(base_, ea_x(), __builtin_bswap32(R(rd).u32)); return true;  // stwbrx
      case 535: F(rd).u64 = SingleToDoubleBits(LoadBE<uint32_t>(base_, ea_x())); return true;  // lfsx
      case 567: { const uint32_t ea = ea_ux(); F(rd).u64 = SingleToDoubleBits(LoadBE<uint32_t>(base_, ea)); R(ra).u64 = ea; return true; }
      case 599: F(rd).u64 = LoadBE<uint64_t>(base_, ea_x()); return true;               // lfdx
      case 631: { const uint32_t ea = ea_ux(); F(rd).u64 = LoadBE<uint64_t>(base_, ea); R(ra).u64 = ea; return true; }
      case 663: StoreBE<uint32_t>(base_, ea_x(), DoubleToSingleBits(F(rd).u64)); return true;  // stfsx
      case 695: { const uint32_t ea = ea_ux(); StoreBE<uint32_t>(base_, ea, DoubleToSingleBits(F(rd).u64)); R(ra).u64 = ea; return true; }
      case 727: StoreBE<uint64_t>(base_, ea_x(), F(rd).u64); return true;               // stfdx
      case 759: { const uint32_t ea = ea_ux(); StoreBE<uint64_t>(base_, ea, F(rd).u64); R(ra).u64 = ea; return true; }
      case 983: StoreBE<uint32_t>(base_, ea_x(), F(rd).u32); return true;               // stfiwx
      case 103: case 359: LoadVector(rd, ea_x() & ~15u); return true;                   // lvx, lvxl
      case 231: case 487: StoreVector(rd, ea_x() & ~15u); return true;                  // stvx, stvxl
      case 519: LoadVectorLeft(rd, ea_x()); return true;                                // lvlx
      case 551: LoadVectorRight(rd, ea_x()); return true;                               // lvrx
      case 647: StoreVectorLeft(rd, ea_x()); return true;                               // stvlx
      case 679: StoreVectorRight(rd, ea_x()); return true;                              // stvrx
      case 339: {  // mfspr
        const uint32_t spr = ((insn >> 16) & 31) | (((insn >> 11) & 31) << 5);
        if (spr == 8) R(rd).u64 = ctx_.lr;
        else if (spr == 9) R(rd).u64 = ctx_.ctr.u64;
        else if (spr == 1)
          R(rd).u64 = (uint64_t(ctx_.xer.so) << 31) | (uint64_t(ctx_.xer.ov) << 30) |
                      (uint64_t(ctx_.xer.ca) << 29);
        else return false;
        return true;
      }
      case 467: {  // mtspr
        const uint32_t spr = ((insn >> 16) & 31) | (((insn >> 11) & 31) << 5);
        if (spr == 8) ctx_.lr = R(rd).u64;
        else if (spr == 9) ctx_.ctr.u64 = R(rd).u64;
        else if (spr == 1) {
          ctx_.xer.so = (R(rd).u32 >> 31) & 1;
          ctx_.xer.ov = (R(rd).u32 >> 30) & 1;
          ctx_.xer.ca = (R(rd).u32 >> 29) & 1;
        } else return false;
        return true;
      }
      case 19: {  // mfcr
        uint64_t cr = 0;
        for (int i = 0; i < 8; ++i)
          cr |= uint64_t(Cr(i).raw()) << (28 - 4 * i);
        R(rd).u64 = cr;
        return true;
      }
      case 144: {  // mtcrf
        const uint32_t crm = (insn >> 12) & 0xFF;
        for (int i = 0; i < 8; ++i)
          if (crm & (0x80 >> i))
            Cr(i).set_raw((R(rd).u32 >> (28 - 4 * i)) & 15);
        return true;
      }
      case 598:  // sync, lwsync
      case 854:  // eieio
      case 86:   // dcbf
      case 54:   // dcbst
      case 278:  // dcbt
      case 246:  // dcbtst
        return true;
      default:
        return false;
    }
    if (record) SetCr0(R(ra).u64);
    return true;
  }

  // Scalar FP --------------------------------------------------------------

  bool StepFpSingle(uint32_t insn) {
    if (insn & 1)
      return false;  // FP record forms set CR1 from FPSCR, not modelled
    const uint32_t d = (insn >> 21) & 31, a = (insn >> 16) & 31, b = (insn >> 11) & 31,
                   c = (insn >> 6) & 31;
    const double fa = F(a).f64, fb = F(b).f64, fc = F(c).f64;
    const uint32_t xo = (insn >> 1) & 31;
    // Operands each form reads, as a mask of frA, frB, frC.
    int uses;
    switch (xo) {
      case 21: case 20: case 18: uses = 3; break;
      case 25: uses = 5; break;
      case 22: case 24: uses = 2; break;
      case 29: case 28: case 31: case 30: uses = 7; break;
      default: return false;
    }
    double r;
    if (PickNaN(fa, fb, fc, uses, &r)) {
      F(d).f64 = RoundToSingle(r);
      return true;
    }
    switch (xo) {
      case 21: r = RoundToSingle(fa + fb); break;            // fadds
      case 20: r = RoundToSingle(fa - fb); break;            // fsubs
      case 25: r = RoundToSingle(fa * fc); break;            // fmuls
      case 18: r = RoundToSingle(fa / fb); break;            // fdivs
      case 22: r = RoundToSingle(std::sqrt(fb)); break;      // fsqrts
      case 24: r = RoundToSingle(1.0 / fb); break;           // fres (exact; see header)
      case 29: r = FmaSingle(fa, fc, fb); break;             // fmadds
      case 28: r = FmaSingle(fa, fc, -fb); break;            // fmsubs
      case 31: r = -FmaSingle(fa, fc, fb); break;            // fnmadds
      default: r = -FmaSingle(fa, fc, -fb); break;           // fnmsubs
    }
    F(d).f64 = FixNaN(r);
    return true;
  }

  bool StepFpDouble(uint32_t insn) {
    const uint32_t d = (insn >> 21) & 31, a = (insn >> 16) & 31, b = (insn >> 11) & 31,
                   c = (insn >> 6) & 31;
    const double fa = F(a).f64, fb = F(b).f64, fc = F(c).f64;
    // A-form (5-bit extended opcode) first.
    const uint32_t xo5 = (insn >> 1) & 31;
    if (xo5 >= 18) {
      if (insn & 1)
        return false;
      double r;
      int uses = 0;
      switch (xo5) {
        case 21: case 20: case 18: uses = 3; break;
        case 25: uses = 5; break;
        case 22: case 26: uses = 2; break;
        case 29: case 28: case 31: case 30: uses = 7; break;
      }
      if (uses && PickNaN(fa, fb, fc, uses, &r)) {
        F(d).f64 = r;
        return true;
      }
      switch (xo5) {
        case 21: r = fa + fb; break;                         // fadd
        case 20: r = fa - fb; break;                         // fsub
        case 25: r = fa * fc; break;                         // fmul
        case 18: r = fa / fb; break;                         // fdiv
        case 22: r = std::sqrt(fb); break;                   // fsqrt
        case 26: r = 1.0 / std::sqrt(fb); break;             // frsqrte (exact)
        case 23: r = (fa >= 0.0) ? fc : fb; break;           // fsel (NaN picks frB)
        case 29: r = std::fma(fa, fc, fb); break;            // fmadd
        case 28: r = std::fma(fa, fc, -fb); break;           // fmsub
        case 31: r = -std::fma(fa, fc, fb); break;           // fnmadd
        case 30: r = -std::fma(fa, fc, -fb); break;          // fnmsub
        default: return false;
      }
      F(d).f64 = xo5 == 23 ? r : FixNaN(r);
      return true;
    }
    const uint32_t xo = (insn >> 1) & 0x3FF;
    switch (xo) {
      case 0: case 32: {  // fcmpu, fcmpo
        PPCCRRegister& f = Cr(d >> 2);
        const bool un = std::isnan(fa) || std::isnan(fb);
        f.lt = !un && fa < fb;
        f.gt = !un && fa > fb;
        f.eq = !un && fa == fb;
        f.un = un;
        return true;
      }
      case 12:  // frsp
        if (insn & 1) return false;
        F(d).f64 = RoundToSingle(fb);
        return true;
      case 72:  // fmr
        if (insn & 1) return false;
        F(d).u64 = F(b).u64;
        return true;
      case 40:  // fneg
        if (insn & 1) return false;
        F(d).u64 = F(b).u64 ^ 0x8000000000000000ull;
        return true;
      case 264:  // fabs
        if (insn & 1) return false;
        F(d).u64 = F(b).u64 & 0x7FFFFFFFFFFFFFFFull;
        return true;
      case 136:  // fnabs
        if (insn & 1) return false;
        F(d).u64 = F(b).u64 | 0x8000000000000000ull;
        return true;
      case 814:  // fctid
        if (insn & 1) return false;
        F(d).u64 = ConvertToInt64(fb, false);
        return true;
      case 815:  // fctidz
        if (insn & 1) return false;
        F(d).u64 = ConvertToInt64(fb, true);
        return true;
      case 14:  // fctiw
        if (insn & 1) return false;
        F(d).u64 = ConvertToInt32(fb, false);
        return true;
      case 15:  // fctiwz
        if (insn & 1) return false;
        F(d).u64 = ConvertToInt32(fb, true);
        return true;
      case 846:  // fcfid
        if (insn & 1) return false;
        F(d).f64 = double(F(b).s64);
        return true;
    }
    return false;
  }

  // VMX (primary 4) --------------------------------------------------------

  bool StepVmx(uint32_t insn) {
    const bool flush = config_.vmx_flush_denormals;
    // VMX128 loads and stores: |4|VD128l|RA|RB|xo|VD128h|11|.
    if ((insn & 3) == 3) {
      const uint32_t vd = ((insn >> 21) & 31) | (((insn >> 2) & 3) << 5);
      const uint32_t ra = (insn >> 16) & 31, rb = (insn >> 11) & 31;
      const uint32_t ea = uint32_t(RA0(ra) + R(rb).u64);
      switch (insn & 0x7F3) {
        case 0x0C3: case 0x2C3: LoadVector(vd, ea & ~15u); return true;    // lvx128, lvxl128
        case 0x1C3: case 0x3C3: StoreVector(vd, ea & ~15u); return true;   // stvx128, stvxl128
        case 0x403: LoadVectorLeft(vd, ea); return true;                   // lvlx128
        case 0x443: LoadVectorRight(vd, ea); return true;                  // lvrx128
        case 0x503: StoreVectorLeft(vd, ea); return true;                  // stvlx128
        case 0x543: StoreVectorRight(vd, ea); return true;                 // stvrx128
      }
      return false;
    }
    const uint32_t vd = (insn >> 21) & 31, va = (insn >> 16) & 31, vb = (insn >> 11) & 31,
                   vc = (insn >> 6) & 31;
    // VA form (6-bit extended opcode).
    switch (insn & 0x3F) {
      case 46:  // vmaddfp vD = vA * vC + vB (fused)
        VmxFloat3(V(vd), V(va), V(vc), V(vb), flush, kNanAcb,
                  [](float a, float c, float b) { return std::fmaf(a, c, b); });
        return true;
      case 47:  // vnmsubfp vD = -(vA * vC - vB)
        VmxFloat3(V(vd), V(va), V(vc), V(vb), flush, kNanAcb,
                  [](float a, float c, float b) { return -std::fmaf(a, c, -b); });
        return true;
      case 42: {  // vsel
        PPCVRegister r;
        for (int i = 0; i < 2; ++i)
          r.u64[i] = (V(vb).u64[i] & V(vc).u64[i]) | (V(va).u64[i] & ~V(vc).u64[i]);
        V(vd) = r;
        return true;
      }
      case 43: {  // vperm
        PPCVRegister r;
        for (int i = 0; i < 16; ++i) {
          const uint32_t sel = V(vc).u8[15 - i] & 31;
          r.u8[15 - i] = sel < 16 ? V(va).u8[15 - sel] : V(vb).u8[15 - (sel - 16)];
        }
        V(vd) = r;
        return true;
      }
      case 44: {  // vsldoi
        const uint32_t sh = (insn >> 6) & 15;
        PPCVRegister r;
        for (int i = 0; i < 16; ++i) {
          const uint32_t j = i + sh;
          r.u8[15 - i] = j < 16 ? V(va).u8[15 - j] : V(vb).u8[15 - (j - 16)];
        }
        V(vd) = r;
        return true;
      }
    }
    // VX form (11-bit extended opcode); VC form compares carry Rc at bit 10.
    const uint32_t xo11 = insn & 0x7FF;
    const uint32_t xo10 = insn & 0x3FF;
    switch (xo10) {
      case 198: return VmxCompareFloat(vd, va, vb, insn & 0x400, 0);  // vcmpeqfp
      case 454: return VmxCompareFloat(vd, va, vb, insn & 0x400, 1);  // vcmpgefp
      case 710: return VmxCompareFloat(vd, va, vb, insn & 0x400, 2);  // vcmpgtfp
      case 134: {  // vcmpequw
        PPCVRegister r;
        for (int i = 0; i < 4; ++i)
          r.u32[i] = V(va).u32[i] == V(vb).u32[i] ? ~0u : 0u;
        V(vd) = r;
        if (insn & 0x400) SetCr6(r);
        return true;
      }
    }
    switch (xo11) {
      case 10:  // vaddfp
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) { return a + b; });
        return true;
      case 74:  // vsubfp
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) { return a - b; });
        return true;
      case 1034:  // vmaxfp
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) {
          if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<float>::quiet_NaN();
          if (a == b) return std::signbit(a) ? b : a;  // max(-0, +0) = +0
          return a > b ? a : b;
        });
        return true;
      case 1098:  // vminfp
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) {
          if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<float>::quiet_NaN();
          if (a == b) return std::signbit(a) ? a : b;  // min(-0, +0) = -0
          return a < b ? a : b;
        });
        return true;
      case 1028: V(vd).u64[0] = V(va).u64[0] & V(vb).u64[0]; V(vd).u64[1] = V(va).u64[1] & V(vb).u64[1]; return true;     // vand
      case 1092: V(vd).u64[0] = V(va).u64[0] & ~V(vb).u64[0]; V(vd).u64[1] = V(va).u64[1] & ~V(vb).u64[1]; return true;   // vandc
      case 1156: V(vd).u64[0] = V(va).u64[0] | V(vb).u64[0]; V(vd).u64[1] = V(va).u64[1] | V(vb).u64[1]; return true;     // vor
      case 1220: V(vd).u64[0] = V(va).u64[0] ^ V(vb).u64[0]; V(vd).u64[1] = V(va).u64[1] ^ V(vb).u64[1]; return true;     // vxor
      case 1284: V(vd).u64[0] = ~(V(va).u64[0] | V(vb).u64[0]); V(vd).u64[1] = ~(V(va).u64[1] | V(vb).u64[1]); return true;  // vnor
      case 652: {  // vspltw
        const uint32_t w = GuestWord(V(vb), va & 3);
        for (int i = 0; i < 4; ++i) V(vd).u32[i] = w;
        return true;
      }
      case 908: {  // vspltisw
        const uint32_t w = uint32_t(Exts(va, 5));
        for (int i = 0; i < 4; ++i) V(vd).u32[i] = w;
        return true;
      }
      case 140: {  // vmrghw
        PPCVRegister r;
        SetGuestWord(r, 0, GuestWord(V(va), 0));
        SetGuestWord(r, 1, GuestWord(V(vb), 0));
        SetGuestWord(r, 2, GuestWord(V(va), 1));
        SetGuestWord(r, 3, GuestWord(V(vb), 1));
        V(vd) = r;
        return true;
      }
      case 396: {  // vmrglw
        PPCVRegister r;
        SetGuestWord(r, 0, GuestWord(V(va), 2));
        SetGuestWord(r, 1, GuestWord(V(vb), 2));
        SetGuestWord(r, 2, GuestWord(V(va), 3));
        SetGuestWord(r, 3, GuestWord(V(vb), 3));
        V(vd) = r;
        return true;
      }
      case 842: {  // vcfsx
        PPCVRegister r;
        for (int i = 0; i < 4; ++i)
          r.u32[i] = FloatToBits(float(std::ldexp(double(V(vb).s32[i]), -int(va))));
        V(vd) = r;
        return true;
      }
      case 778: {  // vcfux
        PPCVRegister r;
        for (int i = 0; i < 4; ++i)
          r.u32[i] = FloatToBits(float(std::ldexp(double(V(vb).u32[i]), -int(va))));
        V(vd) = r;
        return true;
      }
      case 330: {  // vrsqrtefp (exact; see header)
        PPCVRegister r;
        for (int i = 0; i < 4; ++i)
          r.u32[i] = FloatToBits(RsqrtLane(V(vb).u32[i]));
        V(vd) = r;
        return true;
      }
      case 266: {  // vrefp (exact)
        PPCVRegister r;
        for (int i = 0; i < 4; ++i) {
          const uint32_t bits = V(vb).u32[i];
          const float x = Flush(BitsToFloat(bits), flush);
          const float y = Flush(float(1.0 / double(x)), flush);
          r.u32[i] = std::isnan(x) ? bits | 0x00400000u : FloatToBits(y);
        }
        V(vd) = r;
        return true;
      }
    }
    return false;
  }

  float RsqrtLane(uint32_t bits) {
    const bool flush = config_.vmx_flush_denormals;
    const float x = Flush(BitsToFloat(bits), flush);
    if (std::isnan(x))  // the operand's NaN, quieted
      return BitsToFloat(bits | 0x00400000u);
    if (x < 0.0f)
      return BitsToFloat(VmxNaN());
    if (x == 0.0f)
      return std::copysign(std::numeric_limits<float>::infinity(), x);
    return Flush(float(1.0 / std::sqrt(double(x))), flush);
  }

  void SetCr6(const PPCVRegister& mask) {
    bool all = true, none = true;
    for (int i = 0; i < 4; ++i) {
      all &= mask.u32[i] == ~0u;
      none &= mask.u32[i] == 0;
    }
    Cr(6).lt = all;
    Cr(6).gt = 0;
    Cr(6).eq = none;
    Cr(6).so = 0;
  }

  bool VmxCompareFloat(uint32_t vd, uint32_t va, uint32_t vb, bool record, int kind) {
    const bool flush = config_.vmx_flush_denormals;
    PPCVRegister r;
    for (int i = 0; i < 4; ++i) {
      const float a = Flush(BitsToFloat(V(va).u32[i]), flush);
      const float b = Flush(BitsToFloat(V(vb).u32[i]), flush);
      const bool t = kind == 0 ? a == b : kind == 1 ? a >= b : a > b;
      r.u32[i] = t ? ~0u : 0u;
    }
    V(vd) = r;
    if (record)
      SetCr6(r);
    return true;
  }

  // VMX128 arithmetic (primary 5): |5|VD128l|VA128l|VB128l|A|xo|a|1|VD128h|VB128h|.
  bool StepVmx128Arith(uint32_t insn) {
    const bool flush = config_.vmx_flush_denormals;
    const uint32_t vd = ((insn >> 21) & 31) | (((insn >> 2) & 3) << 5);
    const uint32_t va = ((insn >> 16) & 31) | (((insn >> 5) & 1) << 5) | (((insn >> 10) & 1) << 6);
    const uint32_t vb = ((insn >> 11) & 31) | ((insn & 3) << 5);
    switch (insn & 0x3D0) {
      case 0x010:  // vaddfp128
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) { return a + b; });
        return true;
      case 0x050:  // vsubfp128
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) { return a - b; });
        return true;
      case 0x090:  // vmulfp128
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) { return a * b; });
        return true;
      case 0x0D0:  // vmaddfp128 vD = vA * vB + vD
        VmxFloat3(V(vd), V(va), V(vb), V(vd), flush, kNanInOrder,
                  [](float a, float b, float d) { return std::fmaf(a, b, d); });
        return true;
      case 0x110:  // vmaddcfp128 vD = vA * vD + vB
        VmxFloat3(V(vd), V(va), V(vd), V(vb), flush, kNanInOrder,
                  [](float a, float d, float b) { return std::fmaf(a, d, b); });
        return true;
      case 0x150:  // vnmsubfp128 vD = -(vA * vB - vD)
        VmxFloat3(V(vd), V(va), V(vb), V(vd), flush, kNanInOrder,
                  [](float a, float b, float d) { return -std::fmaf(a, b, -d); });
        return true;
      case 0x190:  // vmsum3fp128
        return VmxDot(vd, va, vb, 3);
      case 0x1D0:  // vmsum4fp128
        return VmxDot(vd, va, vb, 4);
      case 0x210: V(vd).u64[0] = V(va).u64[0] & V(vb).u64[0]; V(vd).u64[1] = V(va).u64[1] & V(vb).u64[1]; return true;     // vand128
      case 0x250: V(vd).u64[0] = V(va).u64[0] & ~V(vb).u64[0]; V(vd).u64[1] = V(va).u64[1] & ~V(vb).u64[1]; return true;   // vandc128
      case 0x290: V(vd).u64[0] = ~(V(va).u64[0] | V(vb).u64[0]); V(vd).u64[1] = ~(V(va).u64[1] | V(vb).u64[1]); return true;  // vnor128
      case 0x2D0: V(vd).u64[0] = V(va).u64[0] | V(vb).u64[0]; V(vd).u64[1] = V(va).u64[1] | V(vb).u64[1]; return true;     // vor128
      case 0x310: V(vd).u64[0] = V(va).u64[0] ^ V(vb).u64[0]; V(vd).u64[1] = V(va).u64[1] ^ V(vb).u64[1]; return true;     // vxor128
      case 0x350: {  // vsel128 vD = (vA & ~vD) | (vB & vD)
        PPCVRegister r;
        for (int i = 0; i < 2; ++i)
          r.u64[i] = (V(vb).u64[i] & V(vd).u64[i]) | (V(va).u64[i] & ~V(vd).u64[i]);
        V(vd) = r;
        return true;
      }
    }
    return false;
  }

  // Dot products in Xenon's order, (x + z) + y and (x + z) + (y + w), in
  // double, narrowed once; an overflow of finite inputs gives QNaN and a
  // denormal result flushes to a signed zero (hardware-verified in xenia).
  bool VmxDot(uint32_t vd, uint32_t va, uint32_t vb, int n) {
    const bool flush = config_.vmx_flush_denormals;
    double p[4];
    for (int i = 0; i < 4; ++i)
      p[i] = double(Flush(BitsToFloat(GuestWord(V(va), i)), flush)) *
             double(Flush(BitsToFloat(GuestWord(V(vb), i)), flush));
    const double sum = n == 3 ? (p[0] + p[2]) + p[1] : (p[0] + p[2]) + (p[1] + p[3]);
    float r = float(sum);
    uint32_t bits = FloatToBits(r);
    if (std::isinf(r) && !std::isinf(sum))
      bits = VmxNaN();
    else if (std::isnan(r))
      bits = VmxNaN();
    else if ((bits & 0x7F800000u) == 0)
      bits &= 0x80000000u;
    for (int i = 0; i < 4; ++i) V(vd).u32[i] = bits;
    return true;
  }

  // VMX128 compare and unary (primary 6).
  bool StepVmx128Unary(uint32_t insn) {
    const bool flush = config_.vmx_flush_denormals;
    const uint32_t vd = ((insn >> 21) & 31) | (((insn >> 2) & 3) << 5);
    const uint32_t va = ((insn >> 16) & 31) | (((insn >> 5) & 1) << 5) | (((insn >> 10) & 1) << 6);
    const uint32_t vb = ((insn >> 11) & 31) | ((insn & 3) << 5);
    const uint32_t imm = (insn >> 16) & 31;
    // VX128_R compares: Rc is bit 6.
    switch (insn & 0x390) {
      case 0x000: return VmxCompareFloat(vd, va, vb, insn & 0x40, 0);  // vcmpeqfp128
      case 0x080: return VmxCompareFloat(vd, va, vb, insn & 0x40, 1);  // vcmpgefp128
      case 0x100: return VmxCompareFloat(vd, va, vb, insn & 0x40, 2);  // vcmpgtfp128
      case 0x200: {  // vcmpequw128
        PPCVRegister r;
        for (int i = 0; i < 4; ++i)
          r.u32[i] = V(va).u32[i] == V(vb).u32[i] ? ~0u : 0u;
        V(vd) = r;
        if (insn & 0x40) SetCr6(r);
        return true;
      }
    }
    // VX128_P: vpermwi128.
    if ((insn & 0x630) == 0x210) {
      const uint32_t perm = ((insn >> 16) & 31) | (((insn >> 6) & 7) << 5);
      PPCVRegister r;
      for (int i = 0; i < 4; ++i)
        SetGuestWord(r, i, GuestWord(V(vb), (perm >> (6 - 2 * i)) & 3));
      V(vd) = r;
      return true;
    }
    // VX128 two-operand forms on primary 6 (mask 0x3D0).
    switch (insn & 0x3D0) {
      case 0x280:  // vmaxfp128
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) {
          if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<float>::quiet_NaN();
          if (a == b) return std::signbit(a) ? b : a;
          return a > b ? a : b;
        });
        return true;
      case 0x2C0:  // vminfp128
        VmxFloat2(V(vd), V(va), V(vb), flush, [](float a, float b) {
          if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<float>::quiet_NaN();
          if (a == b) return std::signbit(a) ? a : b;
          return a < b ? a : b;
        });
        return true;
      case 0x300: {  // vmrghw128
        PPCVRegister r;
        SetGuestWord(r, 0, GuestWord(V(va), 0));
        SetGuestWord(r, 1, GuestWord(V(vb), 0));
        SetGuestWord(r, 2, GuestWord(V(va), 1));
        SetGuestWord(r, 3, GuestWord(V(vb), 1));
        V(vd) = r;
        return true;
      }
      case 0x340: {  // vmrglw128
        PPCVRegister r;
        SetGuestWord(r, 0, GuestWord(V(va), 2));
        SetGuestWord(r, 1, GuestWord(V(vb), 2));
        SetGuestWord(r, 2, GuestWord(V(va), 3));
        SetGuestWord(r, 3, GuestWord(V(vb), 3));
        V(vd) = r;
        return true;
      }
    }
    // VX128_3 forms (mask 0x7F0).
    switch (insn & 0x7F0) {
      case 0x2B0: {  // vcsxwfp128
        PPCVRegister r;
        for (int i = 0; i < 4; ++i)
          r.u32[i] = FloatToBits(float(std::ldexp(double(V(vb).s32[i]), -int(imm))));
        V(vd) = r;
        return true;
      }
      case 0x2F0: {  // vcuxwfp128
        PPCVRegister r;
        for (int i = 0; i < 4; ++i)
          r.u32[i] = FloatToBits(float(std::ldexp(double(V(vb).u32[i]), -int(imm))));
        V(vd) = r;
        return true;
      }
      case 0x670: {  // vrsqrtefp128 (exact; see header)
        PPCVRegister r;
        for (int i = 0; i < 4; ++i)
          r.u32[i] = FloatToBits(RsqrtLane(V(vb).u32[i]));
        V(vd) = r;
        return true;
      }
      case 0x630: {  // vrefp128 (exact)
        PPCVRegister r;
        for (int i = 0; i < 4; ++i) {
          const uint32_t bits = V(vb).u32[i];
          const float x = Flush(BitsToFloat(bits), flush);
          const float y = Flush(float(1.0 / double(x)), flush);
          r.u32[i] = std::isnan(x) ? bits | 0x00400000u : FloatToBits(y);
        }
        V(vd) = r;
        return true;
      }
      case 0x730: {  // vspltw128
        const uint32_t w = GuestWord(V(vb), imm & 3);
        for (int i = 0; i < 4; ++i) V(vd).u32[i] = w;
        return true;
      }
      case 0x770: {  // vspltisw128
        const uint32_t w = uint32_t(Exts(imm, 5));
        for (int i = 0; i < 4; ++i) V(vd).u32[i] = w;
        return true;
      }
    }
    return false;
  }

  PPCContext& ctx_;
  uint8_t* base_;
  const Config& config_;
};

}  // namespace detail

// Run from `entry` until it returns, leaves the region, or stops. Stores go
// through the reference log (call RefLogBegin first, RefRollback after).
inline Result Run(PPCContext& ctx, uint8_t* base, uint32_t entry, const Config& config) {
  detail::Machine machine(ctx, base, config);
  return machine.Run(entry);
}

}  // namespace rex::ppc::interp
