#version 450
// Transcodes BC1/BC2/BC3 blocks to ASTC 4x4 LDR blocks, one block per
// invocation. Must produce the same bytes as TranscodeBc{1,2,3}ToAstc4x4 in
// src/graphics/texture_transcode_astc.cpp (see there for the block layout).
// Reads block i from the source binding at src_offset and writes block i to
// the destination binding at dst_offset; both are bound to the same scratch
// buffer, so the regions must not overlap.
//
// Build: glslc -fshader-stage=compute -mfmt=c texture_transcode_bc_astc.cs.glsl

layout(local_size_x = 64) in;

layout(push_constant) uniform Constants {
  uint src_offset;   // bytes
  uint dst_offset;   // bytes
  uint block_count;
  uint mode;         // 0 BC1, 1 BC2, 2 BC3
} pc;

layout(set = 1, binding = 0) readonly buffer Source { uint src[]; };
layout(set = 0, binding = 0) writeonly buffer Destination { uint dst[]; };

const uint kUnquant48[48] = uint[48](
    0, 255, 16, 239, 32, 223, 48, 207, 65, 190, 81, 174,
    97, 158, 113, 142, 5, 250, 21, 234, 38, 217, 54, 201,
    70, 185, 86, 169, 103, 152, 119, 136, 11, 244, 27, 228,
    43, 212, 59, 196, 76, 179, 92, 163, 108, 147, 124, 131);

const uint kTritPack[243] = uint[243](
    0, 1, 2, 4, 5, 6, 8, 9, 10, 16, 17, 18, 20, 21, 22, 24, 25, 26, 3, 7, 15, 19, 23, 27,
    12, 13, 14, 32, 33, 34, 36, 37, 38, 40, 41, 42, 48, 49, 50, 52, 53, 54, 56, 57, 58, 35, 39, 47,
    51, 55, 59, 44, 45, 46, 64, 65, 66, 68, 69, 70, 72, 73, 74, 80, 81, 82, 84, 85, 86, 88, 89, 90,
    67, 71, 79, 83, 87, 91, 76, 77, 78, 128, 129, 130, 132, 133, 134, 136, 137, 138, 144, 145, 146, 148, 149, 150,
    152, 153, 154, 131, 135, 143, 147, 151, 155, 140, 141, 142, 160, 161, 162, 164, 165, 166, 168, 169, 170, 176, 177, 178,
    180, 181, 182, 184, 185, 186, 163, 167, 175, 179, 183, 187, 172, 173, 174, 192, 193, 194, 196, 197, 198, 200, 201, 202,
    208, 209, 210, 212, 213, 214, 216, 217, 218, 195, 199, 207, 211, 215, 219, 204, 205, 206, 96, 97, 98, 100, 101, 102,
    104, 105, 106, 112, 113, 114, 116, 117, 118, 120, 121, 122, 99, 103, 111, 115, 119, 123, 108, 109, 110, 224, 225, 226,
    228, 229, 230, 232, 233, 234, 240, 241, 242, 244, 245, 246, 248, 249, 250, 227, 231, 239, 243, 247, 251, 236, 237, 238,
    28, 29, 30, 60, 61, 62, 92, 93, 94, 156, 157, 158, 188, 189, 190, 220, 221, 222, 31, 63, 127, 159, 191, 255,
    252, 253, 254);

uint blk[4];

void PutBits(int pos, uint value, int count) {
  for (int i = 0; i < count; ++i) {
    if (((value >> i) & 1u) != 0u) blk[(pos + i) >> 5] |= 1u << ((pos + i) & 31);
  }
}

int Sum(ivec3 c) { return c.x + c.y + c.z; }

ivec3 Expand565(uint c) {
  int r = int((c >> 11) & 31u), g = int((c >> 5) & 63u), b = int(c & 31u);
  return ivec3((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

const int kWeightLevels[4] = int[4](0, 21, 43, 64);

int NearestWeight(int position64) {
  int best = 0;
  for (int i = 1; i < 4; ++i) {
    if (abs(kWeightLevels[i] - position64) < abs(kWeightLevels[best] - position64)) best = i;
  }
  return best;
}

uint QuantEndpoint(int value) {
  int best = 0;
  for (int q = 1; q < 48; ++q) {
    if (abs(int(kUnquant48[q]) - value) < abs(int(kUnquant48[best]) - value)) best = q;
  }
  return uint(best);
}

// Stream bit k of the weights lands on block bit 127 - k.
void PutWeights(uint weights[32], int count) {
  for (int k = 0; k < count * 2; ++k) {
    if (((weights[k >> 1] >> (k & 1)) & 1u) != 0u) blk[(127 - k) >> 5] |= 1u << ((127 - k) & 31);
  }
}

void PutEndpoints(int pos, uint codes[8]) {
  for (int g = 0; g < 8; g += 5) {
    int key = 0, mul = 1;
    for (int i = 0; i < 5; ++i, mul *= 3) key += mul * (g + i < 8 ? int(codes[g + i] >> 4) : 0);
    uint t = kTritPack[key];
    for (int i = 0; i < 5 && g + i < 8; ++i) {
      PutBits(pos, codes[g + i] & 15u, 4);
      pos += 4;
      int low = i == 0 ? 0 : i == 1 ? 2 : i == 2 ? 4 : i == 3 ? 5 : 7;
      int width = (i == 2 || i == 4) ? 1 : 2;
      PutBits(pos, (t >> low) & ((1u << width) - 1u), width);
      pos += width;
    }
  }
}

// alpha_weights == null is passed as has_alpha == false.
void WriteBlock(ivec3 e0, ivec3 e1, uint colour_weights[16], ivec2 alpha_range,
                bool has_alpha, uint alpha_weights[16]) {
  blk = uint[4](0u, 0u, 0u, 0u);
  if (!has_alpha) {
    if (Sum(e1) < Sum(e0)) {
      ivec3 t = e0; e0 = e1; e1 = t;
      for (int i = 0; i < 16; ++i) colour_weights[i] = 3u - colour_weights[i];
    }
    PutBits(0, 0x042u, 11);
    PutBits(13, 8u, 4);
    PutBits(17, uint(e0.r), 8);
    PutBits(25, uint(e1.r), 8);
    PutBits(33, uint(e0.g), 8);
    PutBits(41, uint(e1.g), 8);
    PutBits(49, uint(e0.b), 8);
    PutBits(57, uint(e1.b), 8);
    uint w[32];
    for (int i = 0; i < 16; ++i) w[i] = colour_weights[i];
    for (int i = 16; i < 32; ++i) w[i] = 0u;
    PutWeights(w, 16);
  } else {
    uint q[8] = uint[8](QuantEndpoint(e0.r), QuantEndpoint(e1.r), QuantEndpoint(e0.g),
                        QuantEndpoint(e1.g), QuantEndpoint(e0.b), QuantEndpoint(e1.b),
                        QuantEndpoint(alpha_range.x), QuantEndpoint(alpha_range.y));
    if (int(kUnquant48[q[1]]) + int(kUnquant48[q[3]]) + int(kUnquant48[q[5]]) <
        int(kUnquant48[q[0]]) + int(kUnquant48[q[2]]) + int(kUnquant48[q[4]])) {
      uint t;
      t = q[0]; q[0] = q[1]; q[1] = t;
      t = q[2]; q[2] = q[3]; q[3] = t;
      t = q[4]; q[4] = q[5]; q[5] = t;
      for (int i = 0; i < 16; ++i) colour_weights[i] = 3u - colour_weights[i];
    }
    PutBits(0, 0x442u, 11);
    PutBits(13, 12u, 4);
    PutEndpoints(17, q);
    PutBits(128 - 64 - 2, 3u, 2);
    uint w[32];
    for (int i = 0; i < 16; ++i) {
      w[2 * i] = colour_weights[i];
      w[2 * i + 1] = alpha_weights[i];
    }
    PutWeights(w, 32);
  }
}

uint Byte(uint base_word, int index) {  // byte `index` of the block at word base_word
  return (src[base_word + uint(index >> 2)] >> (8 * (index & 3))) & 255u;
}

void WithAlpha(int alpha[16], uint colour_word) {
  int lo = 255, hi = 0;
  for (int i = 0; i < 16; ++i) {
    lo = min(lo, alpha[i]);
    hi = max(hi, alpha[i]);
  }
  uint c0 = src[colour_word] & 0xFFFFu, c1 = src[colour_word] >> 16;
  uint indices = src[colour_word + 1];
  const int position[4] = int[4](0, 64, 21, 43);
  uint colour[16], alpha_weights[16];
  for (int i = 0; i < 16; ++i) {
    colour[i] = uint(NearestWeight(position[(indices >> (2 * i)) & 3u]));
    alpha_weights[i] =
        hi == lo ? 0u : uint(NearestWeight((alpha[i] - lo) * 64 / (hi - lo)));
  }
  WriteBlock(Expand565(c0), Expand565(c1), colour, ivec2(lo, hi), true, alpha_weights);
}

void main() {
  // Groups past 65535 wrap into Y: 65535 * 64 invocations per row.
  uint index = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * 4194240u;
  if (index >= pc.block_count) return;
  uint in_bytes = pc.mode == 0u ? 8u : 16u;
  uint base = (pc.src_offset + index * in_bytes) >> 2;
  if (pc.mode == 0u) {
    uint c0 = src[base] & 0xFFFFu, c1 = src[base] >> 16;
    uint indices = src[base + 1];
    bool four_colour = c0 > c1;
    const int position3[4] = int[4](0, 64, 32, 0);
    const int position4[4] = int[4](0, 64, 21, 43);
    uint colour[16], alpha[16];
    bool transparent = false;
    for (int i = 0; i < 16; ++i) {
      uint idx = (indices >> (2 * i)) & 3u;
      colour[i] = uint(NearestWeight(four_colour ? position4[idx] : position3[idx]));
      bool clear = !four_colour && idx == 3u;
      transparent = transparent || clear;
      alpha[i] = clear ? 0u : 3u;
    }
    WriteBlock(Expand565(c0), Expand565(c1), colour, ivec2(0, 255), transparent, alpha);
  } else if (pc.mode == 1u) {
    int alpha[16];
    for (int i = 0; i < 16; ++i) alpha[i] = int((Byte(base, i >> 1) >> (4 * (i & 1))) & 15u) * 17;
    WithAlpha(alpha, base + 2u);
  } else {
    int a0 = int(Byte(base, 0)), a1 = int(Byte(base, 1));
    int ramp[8];
    ramp[0] = a0;
    ramp[1] = a1;
    int steps = a0 > a1 ? 7 : 5;
    for (int i = 1; i < steps; ++i) {
      ramp[1 + i] = a0 > a1 ? ((7 - i) * a0 + i * a1) / 7 : ((5 - i) * a0 + i * a1) / 5;
    }
    if (a0 <= a1) {
      ramp[6] = 0;
      ramp[7] = 255;
    }
    int alpha[16];
    for (int i = 0; i < 16; ++i) {
      int bit = 3 * i + 16;  // alpha indices start at bit 16 of the block
      uint v = (src[base + uint(bit >> 5)] >> (bit & 31)) & 7u;
      if ((bit & 31) > 29) v |= (src[base + uint(bit >> 5) + 1u] << (32 - (bit & 31))) & 7u;
      alpha[i] = ramp[v & 7u];
    }
    WithAlpha(alpha, base + 2u);
  }
  uint out_word = (pc.dst_offset + index * 16u) >> 2;
  dst[out_word] = blk[0];
  dst[out_word + 1u] = blk[1];
  dst[out_word + 2u] = blk[2];
  dst[out_word + 3u] = blk[3];
}
