#version 460

// UP-4: keeps the game's HUD and the host UI out of frame generation.
//
// Mode 0 puts the game's HUD back over the painted HUD-less frame, the
// frame generator having taken its copy without it: where the HUD-less
// guest output's alpha (the HUD's coverage) is set, the guest output with
// the HUD, upscaled bilinearly, as upscale_hud_composite.cs.glsl does. The
// alpha keeps the larger of the two coverages, for mode 1 and the HDR pass.
//
// Mode 1 builds the presented generated frame: the finished real frame
// wherever its alpha says the HUD or the host UI covers it, the generated
// one elsewhere, so neither is interpolated.

layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform FrameGenerationComposeConstants {
  uvec2 output_size;
  // Mode 0: the painted guest output rectangle within the output.
  ivec2 rect_offset;
  vec2 rect_size_inv;
  // The share of the source images that was painted.
  vec2 source_uv_scale;
  uint mode;
  // Mode 1: the coverage above which the real frame is taken.
  float threshold;
} constants;

// Mode 0: the guest output with the HUD. Mode 1: the finished real frame.
layout(set = 0, binding = 0) uniform sampler2D frame;
// Mode 0: the HUD-less guest output.
layout(set = 0, binding = 1) uniform sampler2D hudless;
// Mode 0: a copy of the painted frame. Mode 1: the generated frame.
layout(set = 0, binding = 2, rgba8) uniform image2D target;

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, constants.output_size))) {
    return;
  }
  if (constants.mode == 1u) {
    // The alpha stays the real frame's too: the HDR pass expands highlights
    // only where it is 0.
    vec4 real = texelFetch(frame, ivec2(pixel), 0);
    if (real.a > constants.threshold) {
      imageStore(target, ivec2(pixel), real);
    } else {
      imageStore(target, ivec2(pixel), vec4(imageLoad(target, ivec2(pixel)).rgb, real.a));
    }
    return;
  }
  vec2 rect_uv = (vec2(ivec2(pixel) - constants.rect_offset) + 0.5) * constants.rect_size_inv;
  if (any(lessThan(rect_uv, vec2(0.0))) || any(greaterThan(rect_uv, vec2(1.0)))) {
    return;
  }
  vec2 uv = rect_uv * constants.source_uv_scale;
  float hud = textureLod(hudless, uv, 0.0).a;
  if (hud <= 0.0) {
    return;
  }
  vec4 color = imageLoad(target, ivec2(pixel));
  color.rgb = mix(color.rgb, textureLod(frame, uv, 0.0).rgb, hud);
  color.a = max(color.a, hud);
  imageStore(target, ivec2(pixel), color);
}
