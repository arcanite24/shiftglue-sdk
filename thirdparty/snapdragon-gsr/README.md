# Snapdragon Game Super Resolution

`present_effect gsr` in the Vulkan presenter is Qualcomm's Snapdragon Game
Super Resolution 1, compiled into the presenter's shaders. Nothing here is
built; this folder holds the upstream licence and where the code came from.

| File | Upstream | Licence |
|---|---|---|
| `src/ui/shaders/guest_output_gsr.ps.glsl` | [snapdragon-gsr](https://github.com/SnapdragonGameStudios/snapdragon-gsr) `d926f07`, `sgsr/v1/include/glsl/sgsr1_shader_mobile.frag` | BSD-3-Clause, `LICENSE` |

The shader takes the presenter's push constants and a separate image and
sampler in place of the original's uniforms and combined sampler; GSR's
arithmetic is unchanged, in its RGBA mode (green as luminance). `LICENSE` is
the repository's, unmodified. The shader keeps its own copyright notice.
