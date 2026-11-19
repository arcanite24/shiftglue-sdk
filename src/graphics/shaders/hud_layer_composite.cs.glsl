#version 460
#extension GL_EXT_samplerless_texture_functions : require

// VR's HUD panel (xr_hud_panel): FH1's HUD is drawn into a layer of its own
// (premultiplied color, alpha its coverage) so the headset can show it apart
// from the scene. This puts it back over the guest output, as the title drew
// it into the front buffer, for the window and the headset's screen.

layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform HudLayerCompositeConstants {
  uvec2 size;
} constants;

layout(set = 0, binding = 0) uniform texture2D hud_layer;
layout(set = 0, binding = 1, rgb10_a2) uniform image2D guest_output;

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), constants.size))) {
    return;
  }
  vec4 hud = texelFetch(hud_layer, pixel, 0);
  vec4 color = imageLoad(guest_output, pixel);
  color.rgb = clamp(hud.rgb + color.rgb * (1.0 - clamp(hud.a, 0.0, 1.0)), 0.0, 1.0);
  imageStore(guest_output, pixel, color);
}
