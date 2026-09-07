// FH1 A4A965C189287B99: early depth, color exponent only.
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
[earlydepthstencil]
float4 main(
#ifdef FH1_COLOR_CENTROID
centroid
#endif
float4 color : TEXCOORD0) : SV_Target {
  return color * asfloat(xe_system[15].y);
}
