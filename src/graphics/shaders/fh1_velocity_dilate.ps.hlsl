Texture2DArray<float4> source_texture : register(t0);
SamplerState source_sampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0)
    : SV_Target0 {
  float4 current =
      source_texture.SampleLevel(source_sampler, float3(uv, 0.0), 0.0);
  float4 below = source_texture.SampleLevel(source_sampler, float3(uv, 0.0),
                                             0.0, int2(0, 1));
  if (current.z - below.z > 0.01) {
    current.xy = below.xy;
  }
  return current;
}
