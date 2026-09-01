cbuffer NativeGuestOutputHybridConstants : register(b0) {
  uint2 output_size;
  float agreement_epsilon;
  uint padding;
};

Texture2D<float4> native_output : register(t0);
Texture2D<float4> xenos_output : register(t1);
RWTexture2D<float4> hybrid_output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint2 position = dispatch_thread_id.xy;
  if (any(position >= output_size)) {
    return;
  }
  float4 native_color = native_output.Load(uint3(position, 0));
  float4 xenos_color = xenos_output.Load(uint3(position, 0));
  bool agrees = all(abs(native_color - xenos_color) <= agreement_epsilon);
  hybrid_output[position] = agrees ? native_color : xenos_color;
}
