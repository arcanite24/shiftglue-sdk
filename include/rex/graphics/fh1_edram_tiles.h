#pragma once
// EDRAM tile ownership for the FH1 native executor (NP-9.0): which native
// surface owns each EDRAM tile, whether a tile's stencil may be nonzero, and
// the claims that move tiles between surfaces. API-agnostic: a claim returns
// what the backend must transfer, and the backend does the copies.

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

class Fh1EdramTiles {
 public:
  static constexpr uint32_t kNoOwner = UINT32_MAX;

  // Contiguous claimed tiles that another surface owned.
  struct Run {
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t previous_owner = kNoOwner;
  };

  // Every tile unowned, with a possibly nonzero stencil.
  void Reset() {
    owners_.assign(xenos::kEdramTileCount, kNoOwner);
    stencil_nonzero_.assign(xenos::kEdramTileCount, 1);
    last_claims_.clear();
  }

  uint32_t Owner(uint32_t tile) const { return owners_[tile & kTileMask]; }
  bool StencilNonzero(uint32_t tile) const { return stencil_nonzero_[tile & kTileMask] != 0; }
  // Bumped whenever ownership or stencil state changes.
  uint64_t generation() const { return generation_; }

  // Claims tiles [base, base + length) (with EDRAM address wrapping) for
  // `key`. Returns the runs whose previous owners' contents the backend must
  // transfer into the surface; none without `transfer` (the caller overwrites
  // the tiles, as a resolve clear does) or when `key` already owns the range.
  std::vector<Run> Claim(uint32_t base, uint32_t length, uint32_t key, bool transfer) {
    length = std::min(length, xenos::kEdramTileCount);
    std::vector<Run> runs;
    if (!length) return runs;
    // A surface that already owns the whole range needs no work; its cached
    // claim is dropped whenever another surface takes any of its tiles.
    auto last = last_claims_.find(key);
    if (last != last_claims_.end() && last->second.first == base &&
        last->second.second >= length) {
      return runs;
    }
    for (uint32_t i = 0; i < length; ++i) {
      uint32_t& owner = owners_[(base + i) & kTileMask];
      if (owner == key) continue;
      ++generation_;
      if (owner != kNoOwner) {
        if (!runs.empty() && runs.back().previous_owner == owner &&
            runs.back().first + runs.back().count == base + i) {
          ++runs.back().count;
        } else {
          runs.push_back({base + i, 1, owner});
        }
      }
      owner = key;
    }
    for (const Run& run : runs) last_claims_.erase(run.previous_owner);
    // A partial claim must not extend a cached larger one.
    if (transfer) {
      last_claims_[key] = {base, length};
    } else {
      last_claims_.erase(key);
      runs.clear();
    }
    return runs;
  }

  // How a rectangle of tiles was claimed by ClaimRect.
  struct RectClaim {
    // The rectangle mixes previous owners, or also holds the claimant's own
    // tiles: nothing was claimed, and the caller claims it row by row.
    bool per_row = false;
    // Otherwise the single previous owner whose contents to transfer, if any.
    uint32_t previous_owner = kNoOwner;
  };

  // Claims tile columns [column_first, column_end) of rows [row_first,
  // row_end) of a surface at `base_tiles` with `pitch_tiles` in one piece,
  // when it has a single previous owner (unowned tiles have no content to
  // keep) and none of the claimant's own tiles, which a transfer would
  // overwrite.
  RectClaim ClaimRect(uint32_t base_tiles, uint32_t pitch_tiles, uint32_t key,
                      uint32_t column_first, uint32_t row_first, uint32_t column_end,
                      uint32_t row_end) {
    RectClaim result;
    if (column_first >= column_end || row_first >= row_end) return result;
    auto tile = [&](uint32_t row, uint32_t column) -> uint32_t& {
      return owners_[(base_tiles + row * pitch_tiles + column) & kTileMask];
    };
    uint32_t previous_owner = kNoOwner;
    bool single = true, own = false;
    for (uint32_t row = row_first; single && row < row_end; ++row) {
      for (uint32_t column = column_first; column < column_end; ++column) {
        const uint32_t owner = tile(row, column);
        if (owner == kNoOwner) continue;
        if (owner == key) {
          own = true;
          continue;
        }
        if (previous_owner != kNoOwner && owner != previous_owner) {
          single = false;
          break;
        }
        previous_owner = owner;
      }
    }
    if (!single || (own && previous_owner != kNoOwner)) {
      result.per_row = true;
      return result;
    }
    for (uint32_t row = row_first; row < row_end; ++row) {
      for (uint32_t column = column_first; column < column_end; ++column) {
        tile(row, column) = key;
      }
    }
    ++generation_;
    last_claims_.erase(key);
    if (previous_owner != kNoOwner) last_claims_.erase(previous_owner);
    result.previous_owner = previous_owner;
    return result;
  }

  void MarkStencil(uint32_t base, uint32_t length, bool nonzero) {
    length = std::min(length, xenos::kEdramTileCount);
    ++generation_;
    for (uint32_t i = 0; i < length; ++i) {
      stencil_nonzero_[(base + i) & kTileMask] = nonzero;
    }
  }

  bool AnyStencil(uint32_t base, uint32_t count) const {
    count = std::min(count, xenos::kEdramTileCount);
    for (uint32_t i = 0; i < count; ++i) {
      if (stencil_nonzero_[(base + i) & kTileMask]) return true;
    }
    return false;
  }

 private:
  static constexpr uint32_t kTileMask = xenos::kEdramTileCount - 1;

  std::vector<uint32_t> owners_;
  std::vector<uint8_t> stencil_nonzero_;
  // Per surface, the range it claimed whole last, to skip repeated claims.
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> last_claims_;
  uint64_t generation_ = 1;
};

}  // namespace rex::graphics
