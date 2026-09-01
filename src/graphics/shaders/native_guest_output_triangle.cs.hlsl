cbuffer NativeGuestOutputTriangleConstants : register(b0) {
  uint2 output_size;
  uint phase;
};

RWTexture2D<float4> output_texture : register(u0);

float Edge(float2 a, float2 b, float2 sample_position) {
  float2 edge = b - a;
  float2 offset = sample_position - a;
  return edge.x * offset.y - edge.y * offset.x;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  if (any(dispatch_thread_id.xy >= output_size)) {
    return;
  }

  float2 sample_position =
      (float2(dispatch_thread_id.xy) + 0.5f) / float2(output_size);
  const float2 vertex_a = float2(0.50f, 0.18f);
  const float2 vertex_b = float2(0.18f, 0.82f);
  const float2 vertex_c = float2(0.82f, 0.82f);
  float edge_a = Edge(vertex_a, vertex_b, sample_position);
  float edge_b = Edge(vertex_b, vertex_c, sample_position);
  float edge_c = Edge(vertex_c, vertex_a, sample_position);
  bool inside = edge_a <= 0.0f && edge_b <= 0.0f && edge_c <= 0.0f;

  float pulse = phase == 0 ? 0.08f : 0.15f;
  float3 background = float3(0.015f, 0.025f + pulse, 0.055f);
  float3 triangle_color = float3(
      saturate(1.2f - sample_position.y),
      saturate(sample_position.x + 0.15f),
      saturate(sample_position.y + 0.1f));
  output_texture[dispatch_thread_id.xy] =
      float4(inside ? triangle_color : background, 1.0f);
}
