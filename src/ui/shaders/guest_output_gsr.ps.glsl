#version 460

// present_effect gsr: Snapdragon Game Super Resolution 1 in one pass from the
// effect's input to its output size. Bilinear colour, then where the green
// channel (the luminance stand-in of GSR's RGBA mode) has an edge, its value
// is rebuilt from 12 texels with a Lanczos-like kernel and the change is added
// to all three channels. This is Qualcomm's sgsr1_shader_mobile.frag
// (github.com/SnapdragonGameStudios/snapdragon-gsr d926f07,
// sgsr/v1/include/glsl; thirdparty/snapdragon-gsr) with the presenter's push
// constants and separate image and sampler; the arithmetic is theirs,
// unchanged.
//
// Compile: glslangValidator -V --vn guest_output_gsr_ps
//   -o vulkan_spirv/guest_output_gsr_ps.h guest_output_gsr.ps.glsl
//
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// SPDX-License-Identifier: BSD-3-Clause

precision mediump float;
precision highp int;

layout(push_constant) uniform GsrConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) ivec2 output_offset;
  highp vec2 output_size_inv;
  // GSR's ViewportInfo[0]: the input's texel size (xy) and size (zw).
  highp vec2 input_size_inv;
  highp vec2 input_size;
} constants;

layout(set = 0, binding = 0) uniform mediump texture2D source_texture;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(location = 0) out vec4 output_color;

#define SOURCE sampler2D(source_texture, source_sampler)

// GSR's RGBA operation mode: the green channel stands in for luminance.
const int kMode = 1;
const float kEdgeThreshold = 8.0 / 255.0;
const float kEdgeSharpness = 2.0;

float fastLanczos2(float x) {
  float wA = x - 4.0;
  float wB = x * wA - wA;
  wA *= wA;
  return wB * wA;
}

vec2 weightY(float dx, float dy, float c, float std) {
  float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
  float w = fastLanczos2(x);
  return vec2(w, w * c);
}

void main() {
  highp vec2 texcoord =
      (vec2(ivec2(gl_FragCoord.xy) - constants.output_offset) + 0.5) * constants.output_size_inv;

  vec4 color;
  color.xyz = textureLod(SOURCE, texcoord, 0.0).xyz;

  highp vec2 imgCoord = ((texcoord * constants.input_size) + vec2(-0.5, 0.5));
  highp vec2 imgCoordPixel = floor(imgCoord);
  highp vec2 coord = (imgCoordPixel * constants.input_size_inv);
  vec2 pl = (imgCoord + (-imgCoordPixel));
  vec4 left = textureGather(SOURCE, coord, kMode);

  float edgeVote =
      abs(left.z - left.y) + abs(color[kMode] - left.y) + abs(color[kMode] - left.z);
  if (edgeVote > kEdgeThreshold) {
    coord.x += constants.input_size_inv.x;

    vec4 right = textureGather(SOURCE, coord + vec2(constants.input_size_inv.x, 0.0), kMode);
    vec4 upDown;
    upDown.xy = textureGather(SOURCE, coord + vec2(0.0, -constants.input_size_inv.y), kMode).wz;
    upDown.zw = textureGather(SOURCE, coord + vec2(0.0, constants.input_size_inv.y), kMode).yx;

    float mean = (left.y + left.z + right.x + right.w) * 0.25;
    left = left - vec4(mean);
    right = right - vec4(mean);
    upDown = upDown - vec4(mean);
    color.w = color[kMode] - mean;

    float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) +
                  (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) +
                 (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
    float std = 2.181818 / sum;

    vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);
    aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);
    aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);
    aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);
    aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);
    aWY += weightY(pl.x, pl.y - 1.0, left.y, std);
    aWY += weightY(pl.x, pl.y, left.z, std);
    aWY += weightY(pl.x + 1.0, pl.y, left.w, std);
    aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);
    aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);
    aWY += weightY(pl.x - 2.0, pl.y, right.z, std);
    aWY += weightY(pl.x - 1.0, pl.y, right.w, std);

    float finalY = aWY.y / aWY.x;

    float maxY = max(max(left.y, left.z), max(right.x, right.w));
    float minY = min(min(left.y, left.z), min(right.x, right.w));
    finalY = clamp(kEdgeSharpness * finalY, minY, maxY);

    float deltaY = finalY - color.w;

    // Smooth high contrast input.
    deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

    color.x = clamp((color.x + deltaY), 0.0, 1.0);
    color.y = clamp((color.y + deltaY), 0.0, 1.0);
    color.z = clamp((color.z + deltaY), 0.0, 1.0);
  }

  // The effects write an opaque alpha (the HUD coverage is added after).
  output_color = vec4(color.xyz, 1.0);
}
