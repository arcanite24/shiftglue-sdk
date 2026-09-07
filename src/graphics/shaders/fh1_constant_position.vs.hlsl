// FH1 B6C9863F710683EC: constant guest position, no vertex fetches.
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };

#ifdef FH1_CONSTANT_COLOR_INTERPOLATOR
struct Fh1ConstantVertex {
  float4 color : TEXCOORD0;
  float4 position : SV_Position;
};
Fh1ConstantVertex main(uint vertex_id : SV_VertexID) {
#else
float4 main(uint vertex_id : SV_VertexID) : SV_Position {
#endif
  precise float4 position = float4(0.0, 0.0, 0.0, 1.0);
  if (!(xe_system[0].x & 8u)) position.w = 1.0 / position.w;
  if (xe_system[0].x & 2u) position.xy *= position.w;
  if (xe_system[0].x & 4u) position.z *= position.w;
  precise float3 scaled_position = position.xyz * asfloat(xe_system[8].xyz);
  position.xyz = mad(asfloat(xe_system[9].xyz), position.w, scaled_position);
#ifdef FH1_CONSTANT_COLOR_INTERPOLATOR
  Fh1ConstantVertex output;
  output.color = 0.0;
  output.position = position;
  return output;
#else
  return position;
#endif
}
