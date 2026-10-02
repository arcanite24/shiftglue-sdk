#version 450
// FH1 native executor ownership transfer into a depth destination, second
// step, in one pass: depth and the whole stencil value from the guest EDRAM
// words fh1_native_transfer_words.cs.hlsl computed per destination sample.
// GLSL, not HLSL, because glslang's HLSL front end lacks SV_StencilRef; it
// matches fh1_native_transfer_from_words.ps.hlsl (FH1_DEST_KIND 1) otherwise.
// Needs shader stencil export; replaces the depth pass and the eight
// stencil-bit passes.
//
// Variants: FH1_DEST_MSAA (per-sample shading).

#extension GL_ARB_shader_stencil_export : require

layout(push_constant) uniform Fh1NativeTransferConstants {
  uint fh1_dest_layout;  // Layout of the new owner.
  uint fh1_dest_shape;   // Destination width in pixels | host samples << 16.
  uint fh1_flags;
};

layout(set = 0, binding = 16, std430) readonly buffer Fh1Words { uint fh1_words[]; };

const uint DEPTH_D24FS8 = 1u;

uint LayoutFormat(uint layout_bits) { return (layout_bits >> 23u) & 0xFu; }

// xenos::Float20e4To32.
float Float20e4To32(uint f24) {
  f24 &= 0xFFFFFFu;
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20u;
  if (exponent == 0u && mantissa != 0u) {
    uint lzcnt = 31u - uint(findMSB(mantissa)) - 11u;
    exponent = uint(1 - int(lzcnt));
    mantissa = (mantissa << lzcnt) & 0xFFFFFu;
  }
  return f24 != 0u ? uintBitsToFloat(((exponent + 112u) << 23u) | (mantissa << 3u)) : 0.0f;
}

void main() {
  uvec2 pixel = uvec2(gl_FragCoord.xy);
  uint samples = fh1_dest_shape >> 16u;
#ifdef FH1_DEST_MSAA
  uint host_sample = uint(gl_SampleID);
#else
  uint host_sample = 0u;
#endif
  uint word = fh1_words[(pixel.y * (fh1_dest_shape & 0xFFFFu) + pixel.x) * samples + host_sample];
  uint depth24 = word >> 8u;
  if (LayoutFormat(fh1_dest_layout) == DEPTH_D24FS8) {
    // The host keeps float24 depth halved.
    gl_FragDepth = Float20e4To32(depth24) * 0.5f;
  } else {
    gl_FragDepth = float(depth24) * (1.0f / 16777215.0f);
  }
  gl_FragStencilRefARB = int(word & 0xFFu);
}
