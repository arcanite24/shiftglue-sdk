cbuffer ProceduralFrameAccumulatorConstants : register(b0) {
  uint2 source_size;
  uint destination_row;
  uint storage_row_count;
};

Texture2DMS<float4, 2> source_tile : register(t0);
RWTexture2D<float4> frame_accumulator : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint2 output_position = dispatch_thread_id.xy;
  if (output_position.x >= source_size.x * 2 ||
      output_position.y >= storage_row_count) {
    return;
  }
  uint source_x = output_position.x >> 1;
  uint sample_index = output_position.x & 1;
  frame_accumulator[uint2(output_position.x,
                          destination_row + output_position.y)] =
      source_tile.Load(uint2(source_x, destination_row + output_position.y),
                       sample_index);
}
