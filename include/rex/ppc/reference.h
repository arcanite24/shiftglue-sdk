#pragma once

// Lockstep verify support (codegen option reference_functions /
// reference_sample_every, NEXT_LEVEL_RESEARCH.md NL-0.2). The generator emits a
// second copy of selected leaf functions, `ref_sub_X`, with the reference
// options (every register in ctx, no folding, no fast paths) and with its
// stores routed through RefStore*, which log the old and new bytes. A host
// verifier runs the reference copy on a copy of the registers, rolls the
// stores back, runs the optimized function for real, and compares registers
// and the logged bytes. Header-only; a build without references never calls
// this.

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace rex::ppc {

struct RefWrite {
  volatile void* host;
  uint32_t guest;
  uint8_t size;
  uint8_t old[16];  // up to a 16-byte vector store (stvx)
  uint8_t neu[16];
  // false for a store the optimized function is allowed to leave out (the
  // link-register slot under host_local_lr, a GPR save the optimized form
  // keeps in a host local): rolled back, never compared.
  bool checked;
};

inline std::vector<RefWrite>& RefLog() {
  thread_local std::vector<RefWrite> log;
  return log;
}

// Seeded faults for the region oracle's own check (rex/ppc/oracle.h): while
// set, the first store outside [frame_lo, frame_hi) is dropped, or held back
// until the next such store has been made. The oracle must then report the
// run as a mismatch.
struct RefFault {
  enum Kind : uint8_t { kNone, kDrop, kReorder } kind = kNone;
  uint32_t frame_lo = 0;
  uint32_t frame_hi = 0;
  bool applied = false;
  bool holding = false;
  RefWrite held{};
};

inline RefFault*& RefFaultSlot() {
  thread_local RefFault* fault = nullptr;
  return fault;
}

// Log a store (its old bytes read now) and make it.
inline void RefCommit(RefWrite w) {
  std::memcpy(w.old, const_cast<const void*>(w.host), w.size);
  RefLog().push_back(w);
  std::memcpy(const_cast<void*>(w.host), w.neu, w.size);
}

// True when the fault took the store (dropped it, or made it late).
inline bool RefFaultTake(RefFault& f, const RefWrite& w) {
  const bool visible = w.guest < f.frame_lo || w.guest >= f.frame_hi;
  if (!visible || (f.applied && !f.holding))
    return false;
  if (f.holding) {
    RefCommit(w);
    RefCommit(f.held);
    f.holding = false;
    return true;
  }
  f.applied = true;
  if (f.kind == RefFault::kReorder) {
    f.held = w;
    f.holding = true;
  }
  return true;
}

// A held store with no later one to swap with is made at the end: nothing
// was reordered.
inline void RefFaultFinish(RefFault& f) {
  if (f.holding) {
    RefCommit(f.held);
    f.holding = false;
    f.applied = false;
  }
}

template <typename T>
inline void RefStore(volatile T* host, uint32_t guest, T value, bool checked = true) {
  RefWrite w{};
  w.host = host;
  w.guest = guest;
  w.size = sizeof(T);
  w.checked = checked;
  std::memcpy(w.neu, &value, sizeof(T));
  if (RefFault* f = RefFaultSlot(); f && RefFaultTake(*f, w)) [[unlikely]]
    return;
  RefCommit(w);
}

// A store of `size` bytes (a 16-byte vector store: REX_REF_STORE_V128) whose
// new bytes are already in host order.
inline void RefStoreBytes(void* host, uint32_t guest, const void* value, uint8_t size) {
  RefWrite w{};
  w.host = host;
  w.guest = guest;
  w.size = size;
  w.checked = true;
  std::memcpy(w.neu, value, size);
  if (RefFault* f = RefFaultSlot(); f && RefFaultTake(*f, w)) [[unlikely]]
    return;
  RefCommit(w);
}

inline void RefLogBegin() { RefLog().clear(); }

// Undo the logged stores, newest first.
inline void RefRollback() {
  auto& log = RefLog();
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    std::memcpy(const_cast<void*>(it->host), it->old, it->size);
  }
}

// After the optimized function ran for real, walk the logged stores newest
// first and hand `visit(store, reference, now, mask)` every checked store with
// a byte that differs from what the reference left there. Comparison is per
// byte against the reference's final state: a byte belongs to the newest store
// covering it (a function may write a word several times, or a vector store
// may cover an older 4- or 8-byte one), so `mask[i]` is set only for the bytes
// this store still owns. `reference` holds the reference copy's final bytes
// over the whole store (newer stores overlaid). A byte whose newest store is
// unchecked is never compared.
template <typename Visit>
inline void RefForEachDifference(Visit&& visit) {
  auto& log = RefLog();
  std::vector<std::pair<uintptr_t, const RefWrite*>> newer;
  newer.reserve(log.size());
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(it->host);
    uint8_t reference[16];
    bool mask[16];
    std::memcpy(reference, it->neu, it->size);
    bool owned = false;
    for (int i = 0; i < it->size; ++i) {
      mask[i] = true;
      for (const auto& n : newer) {
        if (at + i >= n.first && at + i < n.first + n.second->size) {
          mask[i] = false;
          reference[i] = n.second->neu[at + i - n.first];
          break;
        }
      }
      owned |= mask[i];
    }
    newer.emplace_back(at, &*it);
    if (!owned || !it->checked)
      continue;
    uint8_t now[16];
    std::memcpy(now, const_cast<const void*>(it->host), it->size);
    bool differs = false;
    for (int i = 0; i < it->size; ++i)
      differs |= mask[i] && now[i] != reference[i];
    if (differs)
      visit(*it, reference, now, mask);
  }
}

// The newest checked store with a byte that differs, or nullptr.
inline const RefWrite* RefFirstDifference() {
  const RefWrite* first = nullptr;
  RefForEachDifference([&](const RefWrite& w, const uint8_t*, const uint8_t*, const bool*) {
    if (!first)
      first = &w;
  });
  return first;
}

// Distance in single-precision ulps between two floats given as bits, 0 for
// two NaNs (sign and payload are not compared), huge for NaN against a number.
inline double RefFloatUlps(uint32_t a, uint32_t b) {
  auto nan = [](uint32_t x) { return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu); };
  if (nan(a) || nan(b))
    return nan(a) && nan(b) ? 0.0 : 1e30;
  auto ordered = [](uint32_t x) {
    return (x & 0x80000000u) ? -int64_t(x & 0x7FFFFFFFu) : int64_t(x);
  };
  const int64_t d = ordered(a) - ordered(b);
  return double(d < 0 ? -d : d);
}

// The same for doubles, in single-precision ulps (a double ulp is 2^-29 of a
// single one at the same exponent), so one tolerance covers both.
inline double RefDoubleUlps(uint64_t a, uint64_t b) {
  constexpr uint64_t kExp = 0x7FF0000000000000ull, kMan = 0x000FFFFFFFFFFFFFull;
  auto nan = [&](uint64_t x) { return (x & kExp) == kExp && (x & kMan); };
  if (nan(a) || nan(b))
    return nan(a) && nan(b) ? 0.0 : 1e30;
  auto ordered = [](uint64_t x) {
    return (x & 0x8000000000000000ull) ? -double(x & 0x7FFFFFFFFFFFFFFFull) : double(x);
  };
  const double d = ordered(a) - ordered(b);
  return (d < 0 ? -d : d) / double(1u << 29);
}

// Every checked final store whose bytes differ, measured as guest floats: a
// 4-byte store as a big-endian float, an 8-byte store as a big-endian double,
// a 16-byte store as four floats. A 1- or 2-byte store that differs is not a
// float value (`structural`).
struct RefMemoryDiff {
  const RefWrite* first = nullptr;  // the newest differing store
  uint32_t differing = 0;           // differing final stores
  bool structural = false;
  double max_ulps = 0;              // in single-precision ulps
  uint32_t worst_guest = 0;         // the word with the largest distance
  uint64_t worst_reference = 0;     // its value from the reference copy (guest order)
  uint64_t worst_optimized = 0;     // and from the optimized function
};

inline RefMemoryDiff RefCompareMemory() {
  RefMemoryDiff diff;
  auto load_be = [](const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i)
      v = (v << 8) | p[i];
    return v;
  };
  auto note = [&](double ulps, uint32_t guest, uint64_t ref, uint64_t opt) {
    if (ulps >= diff.max_ulps) {
      diff.max_ulps = ulps;
      diff.worst_guest = guest;
      diff.worst_reference = ref;
      diff.worst_optimized = opt;
    }
  };
  // A unit (lane) differs when one of the bytes this store owns differs.
  auto unit_differs = [](const uint8_t* ref, const uint8_t* now, const bool* mask, int n) {
    for (int i = 0; i < n; ++i)
      if (mask[i] && ref[i] != now[i])
        return true;
    return false;
  };
  RefForEachDifference([&](const RefWrite& w, const uint8_t* reference, const uint8_t* now,
                           const bool* mask) {
    if (!diff.first)
      diff.first = &w;
    diff.differing++;
    if (w.size == 4 || w.size == 16) {
      for (int lane = 0; lane < w.size; lane += 4) {
        if (!unit_differs(reference + lane, now + lane, mask + lane, 4))
          continue;
        const uint32_t ref = uint32_t(load_be(reference + lane, 4));
        const uint32_t opt = uint32_t(load_be(now + lane, 4));
        // A small integer reads as a denormal float: never call it close.
        auto tiny = [](uint32_t x) { return (x & 0x7F800000u) == 0 && (x & 0x7FFFFFFFu) != 0; };
        if (tiny(ref) || tiny(opt))
          diff.structural = true;
        note(tiny(ref) || tiny(opt) ? 1e30 : RefFloatUlps(ref, opt), w.guest + lane, ref, opt);
      }
    } else if (w.size == 8) {
      const uint64_t ref = load_be(reference, 8), opt = load_be(now, 8);
      auto tiny = [](uint64_t x) {
        return (x & 0x7FF0000000000000ull) == 0 && (x & 0x7FFFFFFFFFFFFFFFull) != 0;
      };
      if (tiny(ref) || tiny(opt))
        diff.structural = true;
      note(tiny(ref) || tiny(opt) ? 1e30 : RefDoubleUlps(ref, opt), w.guest, ref, opt);
    } else {
      diff.structural = true;
      note(1e30, w.guest, load_be(reference, w.size), load_be(now, w.size));
    }
  });
  return diff;
}

}  // namespace rex::ppc
