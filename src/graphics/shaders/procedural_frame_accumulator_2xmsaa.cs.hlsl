cbuffer ProceduralFrameAccumulatorConstants : register(b0) {
  uint2 source_offset;
  uint2 source_extent;
  uint output_width;
  uint destination_row;
  uint copy_row_count;
  uint sample_select;
};

Texture2DMS<float4, 2> source_tile : register(t0);
RWTexture2D<float4> frame_accumulator : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint2 output_position = dispatch_thread_id.xy;
  if (output_position.x >= output_width ||
      output_position.y >= copy_row_count) {
    return;
  }
  bool expand_samples = output_width == source_extent.x * 2;
  uint source_x = expand_samples ? output_position.x >> 1 : output_position.x;
  uint sample_index = expand_samples ? output_position.x & 1 : sample_select & 1;
  uint2 source_position = source_offset + uint2(source_x, output_position.y);
  float4 value = source_tile.Load(source_position, sample_index);
  if (!expand_samples && sample_select >= 4) {
    value = (source_tile.Load(source_position, 0) +
             source_tile.Load(source_position, 1)) * 0.5;
  }
  frame_accumulator[uint2(output_position.x,
                          destination_row + output_position.y)] =
      value;
}
