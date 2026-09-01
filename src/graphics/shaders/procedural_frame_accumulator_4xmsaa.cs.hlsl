cbuffer ProceduralFrameAccumulatorConstants : register(b0) {
  uint2 source_offset;
  uint2 source_extent;
  uint output_width;
  uint destination_row;
  uint copy_row_count;
  uint sample_select;
};

Texture2DMS<float4, 4> source_tile : register(t0);
RWTexture2D<float4> frame_accumulator : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint2 output_position = dispatch_thread_id.xy;
  if (output_position.x >= output_width ||
      output_position.y >= copy_row_count) {
    return;
  }
  uint2 source_position = source_offset + output_position;
  float4 value;
  if (sample_select == 4) {
    value = (source_tile.Load(source_position, 0) +
             source_tile.Load(source_position, 1)) * 0.5;
  } else if (sample_select == 5) {
    value = (source_tile.Load(source_position, 2) +
             source_tile.Load(source_position, 3)) * 0.5;
  } else if (sample_select >= 6) {
    value = (source_tile.Load(source_position, 0) +
             source_tile.Load(source_position, 1) +
             source_tile.Load(source_position, 2) +
             source_tile.Load(source_position, 3)) * 0.25;
  } else {
    value = source_tile.Load(source_position, min(sample_select, 3));
  }
  frame_accumulator[uint2(output_position.x,
                          destination_row + output_position.y)] = value;
}
