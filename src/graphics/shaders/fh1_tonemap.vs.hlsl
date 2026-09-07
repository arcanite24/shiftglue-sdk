struct VertexOutput {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VertexOutput main(uint vertex_id : SV_VertexID) {
  VertexOutput output;
  output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
  output.position = float4(output.uv * float2(2.0, -2.0) +
                               float2(-1.0, 1.0),
                           0.0, 1.0);
  return output;
}
