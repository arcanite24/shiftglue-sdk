cbuffer IsolatedDepthReadbackConstants : register(b0) {
  uint2 source_size;
  uint sample_count;
};

Texture2DMS<float> source_depth : register(t0);
Texture2DMS<uint2> source_stencil : register(t1);
RWByteAddressBuffer output_samples : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint2 position = dispatch_thread_id.xy;
  if (any(position >= source_size)) {
    return;
  }

  uint output_offset = ((position.y * source_size.x + position.x) * sample_count) * 8;
  for (uint sample_index = 0; sample_index < sample_count; ++sample_index) {
    output_samples.Store(output_offset, asuint(source_depth.Load(position, sample_index)));
    output_samples.Store(output_offset + 4, source_stencil.Load(position, sample_index).y & 0xFFu);
    output_offset += 8;
  }
}
