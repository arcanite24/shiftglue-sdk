// FH1 4D5309C9 material shader CAE1DB68AFFA9D3C only.
// Direct form of the title's 10 guest instructions, retaining the host-facing
// bindings and fixed-function alpha behavior required by its captured draws.

SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);

cbuffer xe_system_cbuffer : register(b0) {
  uint4 xe_system[30];
};

cbuffer xe_float_cbuffer : register(b1) {
  float4 c[3];
};

cbuffer xe_fetch_cbuffer : register(b3) {
  uint4 xe_fetch[48];
};

cbuffer xe_descriptor_indices_cbuffer : register(b4) {
  uint4 xe_descriptor_indices;
};

struct Fh1WorldPixel {
  float4 color : SV_Target;
  uint coverage : SV_Coverage;
};

int SignExtend(uint value, uint bits) {
  const uint shift = 32 - bits;
  return int(value << shift) >> shift;
}

float DecodeGammaSigned(float value) {
  value = saturate(value) * 261120.0;
  float scale;
  float bias;
  if (value >= 98304.0) {
    const bool high = value >= 196608.0;
    scale = high ? (1.0 / 128.0) : (1.0 / 256.0);
    bias = high ? -1024.0 : -256.0;
  } else {
    const bool high = value >= 65536.0;
    scale = high ? (1.0 / 512.0) : (1.0 / 1024.0);
    bias = high ? -64.0 : 0.0;
  }
  value = value * scale + bias;
  return (value + trunc(value * scale)) / 1023.0;
}

float4 ApplyTextureSigns(float4 base_value, uint4 signs) {
  [unroll]
  for (uint component = 0; component < 4; ++component) {
    if (signs[component] == 2) {
      base_value[component] = base_value[component] * 2.0 - 1.0;
    } else if (signs[component] == 3) {
      base_value[component] = DecodeGammaSigned(base_value[component]);
    }
  }
  return base_value;
}

bool AlphaTest(float alpha) {
  const uint compare = (xe_system[0].x >> 7) & 7;
  const float reference = asfloat(xe_system[14].x);
  bool passed = compare == 7;
  passed = passed || ((compare & 1u) != 0 && alpha < reference);
  passed = passed || ((compare & 2u) != 0 && alpha == reference);
  passed = passed || ((compare & 4u) != 0 && alpha > reference);
  return passed;
}

uint AlphaToCoverage(float alpha, float2 position) {
  const uint alpha_to_mask = xe_system[14].y;
  uint coverage = ~0u;
  if (alpha_to_mask) {
    uint pattern = uint(position.x);
    pattern = (uint(position.y) & 1u) | ((pattern & 1u) << 1);
    const float offset = float((alpha_to_mask >> (pattern << 1)) & 3u);
    coverage = 0;
    if (xe_system[13].w) {
      if (xe_system[13].z) {
        coverage |= alpha >= mad(offset, -1.0 / 16.0, 0.75) ? 1u : 0u;
        coverage |= alpha >= mad(offset, -1.0 / 16.0, 0.25) ? 2u : 0u;
        coverage |= alpha >= mad(offset, -1.0 / 16.0, 0.50) ? 4u : 0u;
        coverage |= alpha >= mad(offset, -1.0 / 16.0, 1.00) ? 8u : 0u;
      } else {
        coverage |= alpha >= mad(offset, -1.0 / 8.0, 0.50) ? 2u : 0u;
        coverage |= alpha >= mad(offset, -1.0 / 8.0, 1.00) ? 1u : 0u;
      }
    } else {
      coverage = alpha >= mad(offset, -1.0 / 4.0, 1.00) ? 1u : 0u;
    }
  }
  return coverage;
}

Fh1WorldPixel main(float4 texture_coordinate : TEXCOORD0,
                   float4 vertex_color : TEXCOORD1,
                   float4 position : SV_Position) {
  const uint width = (xe_fetch[0].z & 0x1FFFu) + 1;
  const uint height = ((xe_fetch[0].z >> 13) & 0x1FFFu) + 1;
  float2 uv_offset = (1.5 / 1024.0) / float2(width, height);
  float2 uv = (xe_system[13].x & 1u)
                  ? mad(uv_offset, 0.5, texture_coordinate.xy)
                  : texture_coordinate.xy + uv_offset;

  const int exponent = SignExtend((xe_fetch[0].w >> 13) & 0x3Fu, 6);
  const float gradient_scale = exp2(float(exponent) / 32.0);
  const float2 gradient_x = ddx_coarse(uv) * gradient_scale;
  const float2 gradient_y = ddy_coarse(uv) * gradient_scale;

  const uint4 signs = (xe_system[11].x >> uint4(0, 2, 4, 6)) & 3u;
  float4 texel = 0.0;
  if (!all(signs == 1u)) {
    texel = xe_textures_2d[xe_descriptor_indices.z].SampleGrad(
        xe_samplers[xe_descriptor_indices.y], float3(uv, 0.0),
        gradient_x, gradient_y);
  }
  if (any(signs == 1u)) {
    const float4 signed_texel =
        xe_textures_2d[xe_descriptor_indices.w].SampleGrad(
            xe_samplers[xe_descriptor_indices.y], float3(uv, 0.0),
            gradient_x, gradient_y);
    texel = signs == 1u ? signed_texel : texel;
  }
  texel = ApplyTextureSigns(texel, signs);
  texel *= asfloat((uint(exponent + 127) << 23));

  const float luminance = dot(texel.zxy, c[2].xyz);
  const float3 linear_color =
      (luminance + (texel.xyz - luminance) * c[1].x) * vertex_color.xyz;
  const float alpha = texel.w * c[0].y;
  clip(AlphaTest(alpha) ? 1.0 : -1.0);

  Fh1WorldPixel output;
  output.coverage = AlphaToCoverage(alpha, position.xy);
  clip(output.coverage ? 1.0 : -1.0);
  output.color = float4(sqrt(abs(linear_color)), alpha) *
                 asfloat(xe_system[15].y);
  return output;
}
