#pragma once

// The decoder half of the D3D source census (rex/system/d3d_census.h): tags
// every drawn draw by the buffer it is in and the mark of the API path that
// wrote it, and times the decoder thread by those classes. Called by the
// command processor on its decoder thread only. While the census is off,
// IbBegin checks one flag and the other calls test Active() and return.

#include <cstdint>

namespace rex::graphics::d3d_census_decoder {

// True while an indirect buffer the census follows is being decoded.
extern bool g_active;
inline bool Active() { return g_active; }

// An indirect buffer at guest physical `ptr` starts, at nesting depth
// `depth` (1 for a buffer the ring calls).
void IbBegin(uint32_t ptr, uint32_t count, uint32_t depth);
// The buffer started at `depth` ends.
void IbEnd(uint32_t depth);
// A packet with header `packet` starts `offset` bytes into the current
// buffer (its type-3 opcode is counted for the command buffer, NA-2.2).
void Packet(uint32_t offset, uint32_t packet);
// A draw packet passed its predicate; drawn: it is a draw (not an EDRAM copy)
// of the first tile or of an untiled pass; copy: an EDRAM copy of the same.
void Draw(bool drawn, bool copy);
// DL-1.1: the packet being decoded is in a buffer, or after a mark, whose
// drawn draws the census counts as visible to an API-level renderer: a draw
// call (immediate, tiled or internal) or a display list unchanged since its
// previous run or changed only by fixups. Only meaningful while Active().
bool Visible();
// A swap was decoded: publish this frame's counts.
void Swap();

}  // namespace rex::graphics::d3d_census_decoder
