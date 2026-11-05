#pragma once

#include <cstdint>

namespace rex::graphics {

// Host reference for transcoding Xenos DXT blocks to ASTC 4x4 LDR blocks for
// GPUs without BC (Mali). The GPU load shaders must match these bit for bit.
// Both take one block and write 16 ASTC bytes; the 4x4 texel footprint is the
// same, so only the block data changes. The output is lossy: the colour
// weights are 2 bits (ASTC 0, 21, 43, 64 of 64 against BC's thirds) and BC3
// alpha is cut from 8 to 4 levels.

// 8-byte BC1. 4-colour blocks become one RGB plane; 3-colour blocks that use
// the transparent index become RGBA with a second (alpha) weight plane.
void TranscodeBc1ToAstc4x4(const uint8_t bc1[8], uint8_t astc[16]);
// 16-byte BC2 (explicit 4-bit alpha, cut to 4 levels) and BC3: an RGBA block with a separate alpha
// weight plane.
void TranscodeBc2ToAstc4x4(const uint8_t bc2[16], uint8_t astc[16]);
void TranscodeBc3ToAstc4x4(const uint8_t bc3[16], uint8_t astc[16]);

// Decodes only the blocks the functions above write, to RGBA8 (16 texels, row
// major). Used by tests; returns false for any other ASTC block.
bool DecodeTranscodedAstc4x4(const uint8_t astc[16], uint8_t rgba[64]);

}  // namespace rex::graphics
