Texture2DArray<float4> source_texture : register(t0);
SamplerState source_sampler : register(s0);

cbuffer Constants : register(b0) {
  float exposure;
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0)
    : SV_Target0 {
  float4 source = source_texture.SampleLevel(source_sampler,
                                              float3(uv, 0.0), 0.0);
  return float4(sqrt(abs(source.rgb)), source.a) * exposure;
}
