cbuffer NativeGuestOutputRetainedPassConstants : register(b0) {
  uint2 output_size;
  uint2 source_size;
  uint2 crop_size;
  uint presentation_mode;
  uint padding;
};

Texture2D<float4> retained_pass : register(t0);
RWTexture2D<float4> output_texture : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint2 output_position = dispatch_thread_id.xy;
  if (any(output_position >= output_size)) {
    return;
  }

  if (presentation_mode == 1) {
    if (!all(crop_size) || any(crop_size > source_size)) {
      return;
    }
    uint2 source_position = min(
        output_position * crop_size / output_size, crop_size - 1);
    output_texture[output_position] =
        float4(retained_pass.Load(uint3(source_position, 0)).rgb, 1.0f);
    return;
  }

  uint preview_size = min(output_size.x, output_size.y);
  uint2 preview_origin = (output_size - preview_size) / 2;
  bool inside = all(output_position >= preview_origin) &&
                all(output_position < preview_origin + preview_size);
  if (!inside || !all(crop_size) || !all(source_size)) {
    float stripe = ((output_position.x / 32 + output_position.y / 32) & 1)
                       ? 0.018f
                       : 0.010f;
    output_texture[output_position] = float4(stripe, stripe, stripe, 1.0f);
    return;
  }

  uint2 preview_position = output_position - preview_origin;
  uint2 source_position = min(
      preview_position * crop_size / preview_size, crop_size - 1);
  float4 color = retained_pass.Load(uint3(source_position, 0));
  bool border = any(preview_position < 2) ||
                any(preview_position >= preview_size - 2);
  output_texture[output_position] =
      border ? float4(1.0f, 0.55f, 0.06f, 1.0f)
             : float4(saturate(color.rgb), 1.0f);
}
