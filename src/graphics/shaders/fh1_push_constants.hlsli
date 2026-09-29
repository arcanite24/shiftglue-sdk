// FH1 native shaders take their constants as push constants on Vulkan
// (compile-fh1-native.sh defines FH1_SPIRV for the SPIR-V build) and as root
// constants (register b0) on D3D12.

#ifndef FH1_PUSH_CONSTANTS_HLSLI_
#define FH1_PUSH_CONSTANTS_HLSLI_

// Declare the constants as
//   FH1_PUSH_CONSTANTS cbuffer Name FH1_CONSTANTS_REGISTER { ... };
#ifdef FH1_SPIRV
#define FH1_PUSH_CONSTANTS [[vk::push_constant]]
#define FH1_CONSTANTS_REGISTER
#else
#define FH1_PUSH_CONSTANTS
#define FH1_CONSTANTS_REGISTER : register(b0)
#endif

#endif  // FH1_PUSH_CONSTANTS_HLSLI_
