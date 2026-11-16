/**
 * @file        rex/system/fh1_occlusion_grid.h
 * @brief       FH1 scene depth tiles for title-side occlusion culling
 *
 * The full-frame renderer's FR-3b (docs/FULL_FRAME_RENDERER.md in Pinyon
 * Shift): the GPU plugin reduces an earlier frame's scene depth to a grid
 * of tiles, reads it back a few frames late and publishes it here with that
 * frame's camera, and the title's own per-pass culling drops items whose
 * box lies behind every tile it covers. Lives in the runtime so the GPU
 * plugin and the title share it.
 */

#pragma once

#include <cstdint>

namespace rex::system {

struct Fh1OcclusionGrid {
  static constexpr uint32_t kMaxTiles = 96 * 64;
  // The executor frame the depth is from.
  uint64_t frame = 0;
  // The static world's world-view-projection (row-major, clip = M * p).
  float wvp[16] = {};
  // Stored depth (1 near, 0 far) as depth_a + depth_b / view z.
  float depth_a = 0.0f;
  float depth_b = 0.0f;
  uint32_t columns = 0;
  uint32_t rows = 0;
  // Per tile, the farthest stored depth (the minimum: 1 is near), row-major
  // from the surface's top row.
  float farthest[kMaxTiles] = {};
};

// Copies the grid into the next slot and makes it the latest.
void PublishFh1OcclusionGrid(const Fh1OcclusionGrid& grid);
// Stops handing out a grid (no scene, a cut, the switch off).
void ClearFh1OcclusionGrid();
// The latest grid, or nullptr. Slots are reused four publishes later, so a
// reader must not keep the pointer past its current item.
const Fh1OcclusionGrid* LatestFh1OcclusionGrid();
// Counts the title side reports, logged by the plugin with its stats.
void CountFh1OcclusionTests(uint32_t tested, uint32_t culled);
void TakeFh1OcclusionCounts(uint64_t& tested, uint64_t& culled);

}  // namespace rex::system
