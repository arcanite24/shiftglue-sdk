# Direct3D 11 (Shader Model 5.0) graphics shader bindings

Generated from the fxc reflection listings of the headers in this folder
(and, for `texture_load_*`, of an opt-in build; see below), compared with
the `d3d12_5_1` headers of the same name. Xenia sources: xenia-canary
22d91c8 (references/upstream/xenia-canary). `compile-d3d11.sh` in
`src/graphics/shaders/` regenerates the headers. The UI shaders
(`guest_output_*`, `immediate_*`) are in `src/ui/shaders/bytecode/d3d11_5_0`.

How to read the tables:

- Register numbers are D3D11 slots: `t` SRV, `u` UAV, `s` sampler, `cb`
  constant buffer. Every d3d12_5_1 shader here uses register space 0 only,
  so no register was remapped: the SM 5.0 slot is the SM 5.1 register.
- Format `byte` with dim `r/o` or `r/w` is a (RW)ByteAddressBuffer: bind a
  raw view (DXGI_FORMAT_R32_TYPELESS with D3D11_BUFFER{EX}_SRV_FLAG_RAW or
  D3D11_BUFFER_UAV_FLAG_RAW) of a buffer created with
  D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS. Dim `buf` is a typed buffer
  view; the format column is the element type the shader declares.
- Only resources the code uses are listed (fxc drops unused ones), so a
  variant may skip a slot its source declares (for example `t1` stencil).
- For each cbuffer: the reflected size (the whole declared struct, a
  multiple of 16) and the range the code reads (`dcl_constantbuffer`, in
  16-byte registers times 16). The bound D3D11 buffer must cover at least
  the read range. Members are in offset order, offsets and sizes in bytes;
  `no` = not read. For `xe_system_cbuffer` bind the SDK system constants
  buffer, not a buffer of the reflected size (see below).
- No pixel shader here has a UAV, so there is no render-target/UAV slot
  sharing to handle. No shader loads from a typed UAV, uses SV_StencilRef or
  uses more than one UAV, and none needs an optional feature (no SFI0
  chunk), so all run on feature level 11_0.

## Differences from the d3d12_5_1 layouts

- `adaptive_quad_hs`, `adaptive_triangle_hs`: cbuffer layout differs from d3d12_5_1: `xe_system_cbuffer` size 592 here, 464 in d3d12_5_1; `xe_system_cbuffer` members only here: `uint xe_textures_resolution_scaled`, `uint4 xe_texture_integer_scale_bits[8]`; `xe_system_cbuffer` members only in d3d12_5_1: `uint xe_textures_resolved`
- `continuous_quad_1cp_hs`, `continuous_quad_4cp_hs`, `continuous_triangle_1cp_hs`, `continuous_triangle_3cp_hs`, `discrete_quad_1cp_hs`, `discrete_quad_4cp_hs`, `discrete_triangle_1cp_hs`, `discrete_triangle_3cp_hs`: cbuffer layout differs from d3d12_5_1: `xe_system_cbuffer` size 592 here, 464 in d3d12_5_1; `xe_system_cbuffer` members only here: `uint xe_textures_resolution_scaled`, `uint4 xe_texture_integer_scale_bits[8]`; `xe_system_cbuffer` members only in d3d12_5_1: `uint xe_textures_resolved`
- `tessellation_adaptive_vs`: cbuffer layout differs from d3d12_5_1: `xe_system_cbuffer` size 592 here, 464 in d3d12_5_1; `xe_system_cbuffer` members only here: `uint xe_textures_resolution_scaled`, `uint4 xe_texture_integer_scale_bits[8]`; `xe_system_cbuffer` members only in d3d12_5_1: `uint xe_textures_resolved`
- `tessellation_indexed_vs`: cbuffer layout differs from d3d12_5_1: `xe_system_cbuffer` size 592 here, 464 in d3d12_5_1; `xe_system_cbuffer` members only here: `uint xe_textures_resolution_scaled`, `uint4 xe_texture_integer_scale_bits[8]`; `xe_system_cbuffer` members only in d3d12_5_1: `uint xe_textures_resolved`

The `xe_system_cbuffer` difference comes from compiling upstream
`xenos_draw.hlsli`, which has drifted from the SDK in members these shaders
do not read. Every member they read lies in the first 32 bytes and is at the
same offset in both (the tables mark which); the
authoritative layout of the system constants is the SDK's, not this table.

`texture_load_*` are not in this folder by default: the upstream source
does not reproduce our d3d12_5_1 code (different tiled address code), so
`compile-d3d11.sh` builds them only with `D3D11_UNVERIFIED_TEXTURE_LOAD=1`.
Their bindings below come from that build. Their reflection matches the
d3d12_5_1 headers exactly: typed `Buffer<uint4>` (`Buffer<uint2>` for
`texture_load_8bpb` and `texture_load_8bpb_scaled`) sources and
`RWBuffer<uint4>`, matching the typed UintPow2 SRV/UAV views the D3D12
texture cache creates (`GetUintPow2DXGIFormat`). That build also has no
typed UAV load, SV_StencilRef or SFI0 chunk.

## Shaders

### `adaptive_quad_hs`, `adaptive_triangle_hs`

Stage: hs_5_0.

Bind the SDK system constants buffer at `cb0` (464 bytes in d3d12_5_1); the table
below is the upstream struct, and only its first 32 bytes are read.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `cb0` | `xe_system_cbuffer` | cbuffer | NA | NA | 1 |

cbuffer `xe_system_cbuffer` (`cb0`): reflected size 592 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint xe_flags` | no |
| 4 | 8 | `float2 xe_tessellation_factor_range` | no |
| 12 | 4 | `uint xe_line_loop_closing_index` | no |
| 16 | 4 | `uint xe_vertex_index_endian` | no |
| 20 | 4 | `uint xe_vertex_index_offset` | yes |
| 24 | 8 | `uint2 xe_vertex_index_min_max` | yes |
| 32 | 96 | `float4 xe_user_clip_planes[6]` | no |
| 128 | 12 | `float3 xe_ndc_scale` | no |
| 140 | 4 | `float xe_point_vertex_diameter_min` | no |
| 144 | 12 | `float3 xe_ndc_offset` | no |
| 156 | 4 | `float xe_point_vertex_diameter_max` | no |
| 160 | 8 | `float2 xe_point_constant_diameter` | no |
| 168 | 8 | `float2 xe_point_screen_diameter_to_ndc_radius` | no |
| 176 | 32 | `uint4 xe_texture_swizzled_signs[2]` | no |
| 208 | 4 | `uint xe_textures_resolution_scaled` | no |
| 212 | 8 | `uint2 xe_sample_count_log2` | no |
| 220 | 4 | `float xe_alpha_test_reference` | no |
| 224 | 4 | `uint xe_alpha_to_mask` | no |
| 228 | 4 | `uint xe_edram_32bpp_tile_pitch_dwords_scaled` | no |
| 232 | 4 | `uint xe_edram_depth_base_dwords_scaled` | no |
| 240 | 16 | `float4 xe_color_exp_bias` | no |
| 256 | 8 | `float2 xe_edram_poly_offset_front` | no |
| 264 | 8 | `float2 xe_edram_poly_offset_back` | no |
| 272 | 32 | `uint4 xe_edram_stencil[2]` | no |
| 304 | 16 | `uint4 xe_edram_rt_base_dwords_scaled` | no |
| 320 | 16 | `uint4 xe_edram_rt_format_flags` | no |
| 336 | 64 | `float4 xe_edram_rt_clamp[4]` | no |
| 400 | 32 | `uint4 xe_edram_rt_keep_mask[2]` | no |
| 432 | 16 | `uint4 xe_edram_rt_blend_factors_ops` | no |
| 448 | 16 | `float4 xe_edram_blend_constant` | no |
| 464 | 128 | `uint4 xe_texture_integer_scale_bits[8]` | no |

### `apply_gamma_pwl_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `xe_apply_gamma_ramp` | texture | uint4 | buf | 1 |
| `t1` | `xe_apply_gamma_source` | texture | float4 | 2d | 1 |
| `u0` | `xe_apply_gamma_dest` | UAV | unorm4 | 2d | 1 |
| `cb0` | `push_consts_xe` | cbuffer | NA | NA | 1 |

cbuffer `push_consts_xe` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 8 | `uint2 xe_apply_gamma_size` | yes |

### `apply_gamma_pwl_fxaa_luma_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `xe_apply_gamma_ramp` | texture | uint4 | buf | 1 |
| `t1` | `xe_apply_gamma_source` | texture | float4 | 2d | 1 |
| `u0` | `xe_apply_gamma_dest` | UAV | float4 | 2d | 1 |
| `cb0` | `push_consts_xe` | cbuffer | NA | NA | 1 |

cbuffer `push_consts_xe` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 8 | `uint2 xe_apply_gamma_size` | yes |

### `apply_gamma_table_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `xe_apply_gamma_ramp` | texture | float4 | buf | 1 |
| `t1` | `xe_apply_gamma_source` | texture | float4 | 2d | 1 |
| `u0` | `xe_apply_gamma_dest` | UAV | unorm4 | 2d | 1 |
| `cb0` | `push_consts_xe` | cbuffer | NA | NA | 1 |

cbuffer `push_consts_xe` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 8 | `uint2 xe_apply_gamma_size` | yes |

### `apply_gamma_table_fxaa_luma_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `xe_apply_gamma_ramp` | texture | float4 | buf | 1 |
| `t1` | `xe_apply_gamma_source` | texture | float4 | 2d | 1 |
| `u0` | `xe_apply_gamma_dest` | UAV | float4 | 2d | 1 |
| `cb0` | `push_consts_xe` | cbuffer | NA | NA | 1 |

cbuffer `push_consts_xe` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 8 | `uint2 xe_apply_gamma_size` | yes |

### `continuous_quad_1cp_hs`, `continuous_quad_4cp_hs`, `continuous_triangle_1cp_hs`, `continuous_triangle_3cp_hs`, `discrete_quad_1cp_hs`, `discrete_quad_4cp_hs`, `discrete_triangle_1cp_hs`, `discrete_triangle_3cp_hs`

Stage: hs_5_0.

Bind the SDK system constants buffer at `cb0` (464 bytes in d3d12_5_1); the table
below is the upstream struct, and only its first 32 bytes are read.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `cb0` | `xe_system_cbuffer` | cbuffer | NA | NA | 1 |

cbuffer `xe_system_cbuffer` (`cb0`): reflected size 592 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint xe_flags` | no |
| 4 | 8 | `float2 xe_tessellation_factor_range` | yes |
| 12 | 4 | `uint xe_line_loop_closing_index` | no |
| 16 | 4 | `uint xe_vertex_index_endian` | no |
| 20 | 4 | `uint xe_vertex_index_offset` | no |
| 24 | 8 | `uint2 xe_vertex_index_min_max` | no |
| 32 | 96 | `float4 xe_user_clip_planes[6]` | no |
| 128 | 12 | `float3 xe_ndc_scale` | no |
| 140 | 4 | `float xe_point_vertex_diameter_min` | no |
| 144 | 12 | `float3 xe_ndc_offset` | no |
| 156 | 4 | `float xe_point_vertex_diameter_max` | no |
| 160 | 8 | `float2 xe_point_constant_diameter` | no |
| 168 | 8 | `float2 xe_point_screen_diameter_to_ndc_radius` | no |
| 176 | 32 | `uint4 xe_texture_swizzled_signs[2]` | no |
| 208 | 4 | `uint xe_textures_resolution_scaled` | no |
| 212 | 8 | `uint2 xe_sample_count_log2` | no |
| 220 | 4 | `float xe_alpha_test_reference` | no |
| 224 | 4 | `uint xe_alpha_to_mask` | no |
| 228 | 4 | `uint xe_edram_32bpp_tile_pitch_dwords_scaled` | no |
| 232 | 4 | `uint xe_edram_depth_base_dwords_scaled` | no |
| 240 | 16 | `float4 xe_color_exp_bias` | no |
| 256 | 8 | `float2 xe_edram_poly_offset_front` | no |
| 264 | 8 | `float2 xe_edram_poly_offset_back` | no |
| 272 | 32 | `uint4 xe_edram_stencil[2]` | no |
| 304 | 16 | `uint4 xe_edram_rt_base_dwords_scaled` | no |
| 320 | 16 | `uint4 xe_edram_rt_format_flags` | no |
| 336 | 64 | `float4 xe_edram_rt_clamp[4]` | no |
| 400 | 32 | `uint4 xe_edram_rt_keep_mask[2]` | no |
| 432 | 16 | `uint4 xe_edram_rt_blend_factors_ops` | no |
| 448 | 16 | `float4 xe_edram_blend_constant` | no |
| 464 | 128 | `uint4 xe_texture_integer_scale_bits[8]` | no |

### `fh1_native_resolve_memory_color_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | float4 | 2d | 1 |
| `u0` | `fh1_memory` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeResolveMemoryConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeResolveMemoryConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_resolve_layout` | yes |
| 12 | 4 | `uint fh1_owner_layout` | yes |
| 16 | 4 | `uint fh1_sample_select` | yes |
| 20 | 4 | `uint fh1_dest_info` | yes |
| 24 | 4 | `uint fh1_dest_base` | yes |
| 28 | 4 | `uint fh1_dest_pitch` | yes |

### `fh1_native_resolve_memory_color_ms_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | float4 | 2dMS | 1 |
| `u0` | `fh1_memory` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeResolveMemoryConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeResolveMemoryConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_resolve_layout` | yes |
| 12 | 4 | `uint fh1_owner_layout` | yes |
| 16 | 4 | `uint fh1_sample_select` | yes |
| 20 | 4 | `uint fh1_dest_info` | yes |
| 24 | 4 | `uint fh1_dest_base` | yes |
| 28 | 4 | `uint fh1_dest_pitch` | yes |

### `fh1_native_resolve_memory_depth_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_depth` | texture | float | 2d | 1 |
| `t1` | `fh1_source_stencil` | texture | uint2 | 2d | 1 |
| `u0` | `fh1_memory` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeResolveMemoryConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeResolveMemoryConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_resolve_layout` | yes |
| 12 | 4 | `uint fh1_owner_layout` | yes |
| 16 | 4 | `uint fh1_sample_select` | yes |
| 20 | 4 | `uint fh1_dest_info` | yes |
| 24 | 4 | `uint fh1_dest_base` | yes |
| 28 | 4 | `uint fh1_dest_pitch` | yes |

### `fh1_native_resolve_memory_depth_ms_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_depth` | texture | float | 2dMS | 1 |
| `t1` | `fh1_source_stencil` | texture | uint2 | 2dMS | 1 |
| `u0` | `fh1_memory` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeResolveMemoryConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeResolveMemoryConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_resolve_layout` | yes |
| 12 | 4 | `uint fh1_owner_layout` | yes |
| 16 | 4 | `uint fh1_sample_select` | yes |
| 20 | 4 | `uint fh1_dest_info` | yes |
| 24 | 4 | `uint fh1_dest_base` | yes |
| 28 | 4 | `uint fh1_dest_pitch` | yes |

### `fh1_native_resolve_memory_uint_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | uint4 | 2d | 1 |
| `u0` | `fh1_memory` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeResolveMemoryConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeResolveMemoryConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_resolve_layout` | yes |
| 12 | 4 | `uint fh1_owner_layout` | yes |
| 16 | 4 | `uint fh1_sample_select` | yes |
| 20 | 4 | `uint fh1_dest_info` | yes |
| 24 | 4 | `uint fh1_dest_base` | yes |
| 28 | 4 | `uint fh1_dest_pitch` | yes |

### `fh1_native_resolve_memory_uint_ms_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | uint4 | 2dMS | 1 |
| `u0` | `fh1_memory` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeResolveMemoryConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeResolveMemoryConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_resolve_layout` | yes |
| 12 | 4 | `uint fh1_owner_layout` | yes |
| 16 | 4 | `uint fh1_sample_select` | yes |
| 20 | 4 | `uint fh1_dest_info` | yes |
| 24 | 4 | `uint fh1_dest_base` | yes |
| 28 | 4 | `uint fh1_dest_pitch` | yes |

### `fh1_native_transfer_color_dms_from_color_ms_ps`, `fh1_native_transfer_color_from_color_ms_ps`, `fh1_native_transfer_depth_dms_from_color_ms_ps`, `fh1_native_transfer_depth_from_color_ms_ps`, `fh1_native_transfer_stencil_dms_from_color_ms_ps`, `fh1_native_transfer_stencil_from_color_ms_ps`, `fh1_native_transfer_uint_dms_from_color_ms_ps`, `fh1_native_transfer_uint_from_color_ms_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | float4 | 2dMS | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_source_layout` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_color_dms_from_color_ps`, `fh1_native_transfer_color_from_color_ps`, `fh1_native_transfer_depth_dms_from_color_ps`, `fh1_native_transfer_depth_from_color_ps`, `fh1_native_transfer_stencil_dms_from_color_ps`, `fh1_native_transfer_stencil_from_color_ps`, `fh1_native_transfer_uint_dms_from_color_ps`, `fh1_native_transfer_uint_from_color_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | float4 | 2d | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_source_layout` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_color_dms_from_depth_ms_ps`, `fh1_native_transfer_color_from_depth_ms_ps`, `fh1_native_transfer_stencil_dms_from_depth_ms_ps`, `fh1_native_transfer_stencil_from_depth_ms_ps`, `fh1_native_transfer_uint_dms_from_depth_ms_ps`, `fh1_native_transfer_uint_from_depth_ms_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_depth` | texture | float | 2dMS | 1 |
| `t1` | `fh1_source_stencil` | texture | uint2 | 2dMS | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_source_layout` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_color_dms_from_depth_ps`, `fh1_native_transfer_color_from_depth_ps`, `fh1_native_transfer_stencil_dms_from_depth_ps`, `fh1_native_transfer_stencil_from_depth_ps`, `fh1_native_transfer_uint_dms_from_depth_ps`, `fh1_native_transfer_uint_from_depth_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_depth` | texture | float | 2d | 1 |
| `t1` | `fh1_source_stencil` | texture | uint2 | 2d | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_source_layout` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_color_dms_from_uint_ms_ps`, `fh1_native_transfer_color_from_uint_ms_ps`, `fh1_native_transfer_depth_dms_from_uint_ms_ps`, `fh1_native_transfer_depth_from_uint_ms_ps`, `fh1_native_transfer_stencil_dms_from_uint_ms_ps`, `fh1_native_transfer_stencil_from_uint_ms_ps`, `fh1_native_transfer_uint_dms_from_uint_ms_ps`, `fh1_native_transfer_uint_from_uint_ms_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | uint4 | 2dMS | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_source_layout` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_color_dms_from_uint_ps`, `fh1_native_transfer_color_from_uint_ps`, `fh1_native_transfer_depth_dms_from_uint_ps`, `fh1_native_transfer_depth_from_uint_ps`, `fh1_native_transfer_stencil_dms_from_uint_ps`, `fh1_native_transfer_stencil_from_uint_ps`, `fh1_native_transfer_uint_dms_from_uint_ps`, `fh1_native_transfer_uint_from_uint_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | uint4 | 2d | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_source_layout` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_depth_dms_from_depth_ms_ps`, `fh1_native_transfer_depth_from_depth_ms_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_depth` | texture | float | 2dMS | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_source_layout` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_depth_dms_from_depth_ps`, `fh1_native_transfer_depth_from_depth_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_depth` | texture | float | 2d | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_source_layout` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_depth_dms_from_words_ps`, `fh1_native_transfer_depth_from_words_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_words` | texture | byte | r/o | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | yes |
| 4 | 4 | `uint fh1_dest_shape` | yes |
| 8 | 4 | `uint fh1_flags` | no |

### `fh1_native_transfer_stencil_dms_from_words_ps`, `fh1_native_transfer_stencil_from_words_ps`

Stage: ps_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_words` | texture | byte | r/o | 1 |
| `cb0` | `Fh1NativeTransferConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_dest_layout` | no |
| 4 | 4 | `uint fh1_dest_shape` | yes |
| 8 | 4 | `uint fh1_flags` | yes |

### `fh1_native_transfer_words_color_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | float4 | 2d | 1 |
| `u0` | `fh1_words` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeTransferWordsConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferWordsConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_dest_layout` | yes |
| 12 | 4 | `uint fh1_source_layout` | yes |
| 16 | 4 | `uint fh1_flags` | yes |
| 20 | 4 | `uint fh1_dest_width` | yes |
| 24 | 4 | `uint fh1_dest_samples` | yes |
| 28 | 4 | `uint fh1_unused` | no |

### `fh1_native_transfer_words_color_ms_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | float4 | 2dMS | 1 |
| `u0` | `fh1_words` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeTransferWordsConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferWordsConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_dest_layout` | yes |
| 12 | 4 | `uint fh1_source_layout` | yes |
| 16 | 4 | `uint fh1_flags` | yes |
| 20 | 4 | `uint fh1_dest_width` | yes |
| 24 | 4 | `uint fh1_dest_samples` | yes |
| 28 | 4 | `uint fh1_unused` | no |

### `fh1_native_transfer_words_depth_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_depth` | texture | float | 2d | 1 |
| `t1` | `fh1_source_stencil` | texture | uint2 | 2d | 1 |
| `u0` | `fh1_words` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeTransferWordsConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferWordsConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_dest_layout` | yes |
| 12 | 4 | `uint fh1_source_layout` | yes |
| 16 | 4 | `uint fh1_flags` | yes |
| 20 | 4 | `uint fh1_dest_width` | yes |
| 24 | 4 | `uint fh1_dest_samples` | yes |
| 28 | 4 | `uint fh1_unused` | no |

### `fh1_native_transfer_words_depth_ms_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_depth` | texture | float | 2dMS | 1 |
| `t1` | `fh1_source_stencil` | texture | uint2 | 2dMS | 1 |
| `u0` | `fh1_words` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeTransferWordsConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferWordsConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_dest_layout` | yes |
| 12 | 4 | `uint fh1_source_layout` | yes |
| 16 | 4 | `uint fh1_flags` | yes |
| 20 | 4 | `uint fh1_dest_width` | yes |
| 24 | 4 | `uint fh1_dest_samples` | yes |
| 28 | 4 | `uint fh1_unused` | no |

### `fh1_native_transfer_words_uint_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | uint4 | 2d | 1 |
| `u0` | `fh1_words` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeTransferWordsConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferWordsConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_dest_layout` | yes |
| 12 | 4 | `uint fh1_source_layout` | yes |
| 16 | 4 | `uint fh1_flags` | yes |
| 20 | 4 | `uint fh1_dest_width` | yes |
| 24 | 4 | `uint fh1_dest_samples` | yes |
| 28 | 4 | `uint fh1_unused` | no |

### `fh1_native_transfer_words_uint_ms_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `fh1_source_color` | texture | uint4 | 2dMS | 1 |
| `u0` | `fh1_words` | UAV | byte | r/w | 1 |
| `cb0` | `Fh1NativeTransferWordsConstants` | cbuffer | NA | NA | 1 |

cbuffer `Fh1NativeTransferWordsConstants` (`cb0`): reflected size 32 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint fh1_rect_origin` | yes |
| 4 | 4 | `uint fh1_rect_size` | yes |
| 8 | 4 | `uint fh1_dest_layout` | yes |
| 12 | 4 | `uint fh1_source_layout` | yes |
| 16 | 4 | `uint fh1_flags` | yes |
| 20 | 4 | `uint fh1_dest_width` | yes |
| 24 | 4 | `uint fh1_dest_samples` | yes |
| 28 | 4 | `uint fh1_unused` | no |

### `fh1_reflection_cube_import_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `source` | texture | uint4 | buf | 1 |
| `u0` | `dest` | UAV | uint4 | 2darray | 1 |
| `cb0` | `Load` | cbuffer | NA | NA | 1 |

cbuffer `Load` (`cb0`): reflected size 48 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint flags` | yes |
| 4 | 4 | `uint guest_offset` | yes |
| 8 | 4 | `uint guest_pitch` | yes |
| 12 | 4 | `uint guest_z_stride` | no |
| 16 | 12 | `uint3 size_blocks` | yes |
| 28 | 4 | `uint face_stride` | yes |
| 32 | 4 | `uint unused_host_pitch` | no |
| 36 | 4 | `uint unused_height` | no |

### `fh1_scaled_32bpp_2x_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `source` | texture | uint4 | buf | 1 |
| `u0` | `dest` | UAV | uint4 | buf | 1 |
| `cb0` | `Load` | cbuffer | NA | NA | 1 |

cbuffer `Load` (`cb0`): reflected size 48 bytes, code reads the first 48 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint flags` | no |
| 4 | 4 | `uint guest_offset` | yes |
| 8 | 4 | `uint guest_pitch` | yes |
| 12 | 4 | `uint guest_z_stride` | no |
| 16 | 12 | `uint3 size_blocks` | yes |
| 28 | 4 | `uint host_offset` | yes |
| 32 | 4 | `uint host_pitch` | yes |
| 36 | 4 | `uint height_texels` | no |

### `float24_round_ps`, `float24_truncate_ps`

Stage: ps_5_0.

No resources.

### `fullscreen_cw_vs`

Stage: vs_5_0.

No resources.

### `fxaa_cs`, `fxaa_extreme_cs`

Stage: cs_5_0.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `s0` | `xe_sampler_linear_clamp` | sampler | NA | NA | 1 |
| `t0` | `xe_fxaa_source` | texture | float4 | 2d | 1 |
| `u0` | `xe_fxaa_dest` | UAV | unorm4 | 2d | 1 |
| `cb0` | `XeApplyGammaRampConstants` | cbuffer | NA | NA | 1 |

cbuffer `XeApplyGammaRampConstants` (`cb0`): reflected size 16 bytes, code reads the first 16 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 8 | `uint2 xe_fxaa_size` | yes |
| 8 | 8 | `float2 xe_fxaa_size_inv` | yes |

### `tessellation_adaptive_vs`

Stage: vs_5_0.

Bind the SDK system constants buffer at `cb0` (464 bytes in d3d12_5_1); the table
below is the upstream struct, and only its first 32 bytes are read.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `cb0` | `xe_system_cbuffer` | cbuffer | NA | NA | 1 |

cbuffer `xe_system_cbuffer` (`cb0`): reflected size 592 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint xe_flags` | no |
| 4 | 8 | `float2 xe_tessellation_factor_range` | yes |
| 12 | 4 | `uint xe_line_loop_closing_index` | no |
| 16 | 4 | `uint xe_vertex_index_endian` | yes |
| 20 | 4 | `uint xe_vertex_index_offset` | no |
| 24 | 8 | `uint2 xe_vertex_index_min_max` | no |
| 32 | 96 | `float4 xe_user_clip_planes[6]` | no |
| 128 | 12 | `float3 xe_ndc_scale` | no |
| 140 | 4 | `float xe_point_vertex_diameter_min` | no |
| 144 | 12 | `float3 xe_ndc_offset` | no |
| 156 | 4 | `float xe_point_vertex_diameter_max` | no |
| 160 | 8 | `float2 xe_point_constant_diameter` | no |
| 168 | 8 | `float2 xe_point_screen_diameter_to_ndc_radius` | no |
| 176 | 32 | `uint4 xe_texture_swizzled_signs[2]` | no |
| 208 | 4 | `uint xe_textures_resolution_scaled` | no |
| 212 | 8 | `uint2 xe_sample_count_log2` | no |
| 220 | 4 | `float xe_alpha_test_reference` | no |
| 224 | 4 | `uint xe_alpha_to_mask` | no |
| 228 | 4 | `uint xe_edram_32bpp_tile_pitch_dwords_scaled` | no |
| 232 | 4 | `uint xe_edram_depth_base_dwords_scaled` | no |
| 240 | 16 | `float4 xe_color_exp_bias` | no |
| 256 | 8 | `float2 xe_edram_poly_offset_front` | no |
| 264 | 8 | `float2 xe_edram_poly_offset_back` | no |
| 272 | 32 | `uint4 xe_edram_stencil[2]` | no |
| 304 | 16 | `uint4 xe_edram_rt_base_dwords_scaled` | no |
| 320 | 16 | `uint4 xe_edram_rt_format_flags` | no |
| 336 | 64 | `float4 xe_edram_rt_clamp[4]` | no |
| 400 | 32 | `uint4 xe_edram_rt_keep_mask[2]` | no |
| 432 | 16 | `uint4 xe_edram_rt_blend_factors_ops` | no |
| 448 | 16 | `float4 xe_edram_blend_constant` | no |
| 464 | 128 | `uint4 xe_texture_integer_scale_bits[8]` | no |

### `tessellation_indexed_vs`

Stage: vs_5_0.

Bind the SDK system constants buffer at `cb0` (464 bytes in d3d12_5_1); the table
below is the upstream struct, and only its first 32 bytes are read.

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `cb0` | `xe_system_cbuffer` | cbuffer | NA | NA | 1 |

cbuffer `xe_system_cbuffer` (`cb0`): reflected size 592 bytes, code reads the first 32 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint xe_flags` | no |
| 4 | 8 | `float2 xe_tessellation_factor_range` | no |
| 12 | 4 | `uint xe_line_loop_closing_index` | no |
| 16 | 4 | `uint xe_vertex_index_endian` | yes |
| 20 | 4 | `uint xe_vertex_index_offset` | yes |
| 24 | 8 | `uint2 xe_vertex_index_min_max` | yes |
| 32 | 96 | `float4 xe_user_clip_planes[6]` | no |
| 128 | 12 | `float3 xe_ndc_scale` | no |
| 140 | 4 | `float xe_point_vertex_diameter_min` | no |
| 144 | 12 | `float3 xe_ndc_offset` | no |
| 156 | 4 | `float xe_point_vertex_diameter_max` | no |
| 160 | 8 | `float2 xe_point_constant_diameter` | no |
| 168 | 8 | `float2 xe_point_screen_diameter_to_ndc_radius` | no |
| 176 | 32 | `uint4 xe_texture_swizzled_signs[2]` | no |
| 208 | 4 | `uint xe_textures_resolution_scaled` | no |
| 212 | 8 | `uint2 xe_sample_count_log2` | no |
| 220 | 4 | `float xe_alpha_test_reference` | no |
| 224 | 4 | `uint xe_alpha_to_mask` | no |
| 228 | 4 | `uint xe_edram_32bpp_tile_pitch_dwords_scaled` | no |
| 232 | 4 | `uint xe_edram_depth_base_dwords_scaled` | no |
| 240 | 16 | `float4 xe_color_exp_bias` | no |
| 256 | 8 | `float2 xe_edram_poly_offset_front` | no |
| 264 | 8 | `float2 xe_edram_poly_offset_back` | no |
| 272 | 32 | `uint4 xe_edram_stencil[2]` | no |
| 304 | 16 | `uint4 xe_edram_rt_base_dwords_scaled` | no |
| 320 | 16 | `uint4 xe_edram_rt_format_flags` | no |
| 336 | 64 | `float4 xe_edram_rt_clamp[4]` | no |
| 400 | 32 | `uint4 xe_edram_rt_keep_mask[2]` | no |
| 432 | 16 | `uint4 xe_edram_rt_blend_factors_ops` | no |
| 448 | 16 | `float4 xe_edram_blend_constant` | no |
| 464 | 128 | `uint4 xe_texture_integer_scale_bits[8]` | no |

### `texture_load_128bpb_cs`, `texture_load_128bpb_scaled_cs`, `texture_load_16bpb_cs`, `texture_load_16bpb_scaled_cs`, `texture_load_32bpb_cs`, `texture_load_32bpb_scaled_cs`, `texture_load_64bpb_cs`, `texture_load_64bpb_scaled_cs`, `texture_load_bgrg8_rgb8_cs`, `texture_load_bgrg8_rgbg8_cs`, `texture_load_depth_float_cs`, `texture_load_depth_float_scaled_cs`, `texture_load_depth_unorm_cs`, `texture_load_depth_unorm_scaled_cs`, `texture_load_gbgr8_grgb8_cs`, `texture_load_gbgr8_rgb8_cs`, `texture_load_r10g11b11_rgba16_cs`, `texture_load_r10g11b11_rgba16_scaled_cs`, `texture_load_r10g11b11_rgba16_snorm_cs`, `texture_load_r10g11b11_rgba16_snorm_scaled_cs`, `texture_load_r11g11b10_rgba16_cs`, `texture_load_r11g11b10_rgba16_scaled_cs`, `texture_load_r11g11b10_rgba16_snorm_cs`, `texture_load_r11g11b10_rgba16_snorm_scaled_cs`, `texture_load_r4g4b4a4_b4g4r4a4_cs`, `texture_load_r4g4b4a4_b4g4r4a4_scaled_cs`, `texture_load_r5g5b5a1_b5g5r5a1_cs`, `texture_load_r5g5b5a1_b5g5r5a1_scaled_cs`, `texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs`, `texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs`, `texture_load_r5g6b5_b5g6r5_cs`, `texture_load_r5g6b5_b5g6r5_scaled_cs`

Stage: cs_5_0. Opt-in, unverified source (see above).

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `xe_texture_load_source` | texture | uint4 | buf | 1 |
| `u0` | `xe_texture_load_dest` | UAV | uint4 | buf | 1 |
| `cb0` | `push_consts_xe` | cbuffer | NA | NA | 1 |

cbuffer `push_consts_xe` (`cb0`): reflected size 48 bytes, code reads the first 48 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint xe_texture_load_is_tiled_3d_endian_scale` | yes |
| 4 | 4 | `uint xe_texture_load_guest_offset` | yes |
| 8 | 4 | `uint xe_texture_load_guest_pitch_aligned` | yes |
| 12 | 4 | `uint xe_texture_load_guest_z_stride_block_rows_aligned` | yes |
| 16 | 12 | `uint3 xe_texture_load_size_blocks` | yes |
| 28 | 4 | `uint xe_texture_load_host_offset` | yes |
| 32 | 4 | `uint xe_texture_load_host_pitch` | yes |
| 36 | 4 | `uint xe_texture_load_height_texels` | no |

### `texture_load_8bpb_cs`, `texture_load_8bpb_scaled_cs`

Stage: cs_5_0. Opt-in, unverified source (see above).

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `xe_texture_load_source` | texture | uint2 | buf | 1 |
| `u0` | `xe_texture_load_dest` | UAV | uint4 | buf | 1 |
| `cb0` | `push_consts_xe` | cbuffer | NA | NA | 1 |

cbuffer `push_consts_xe` (`cb0`): reflected size 48 bytes, code reads the first 48 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint xe_texture_load_is_tiled_3d_endian_scale` | yes |
| 4 | 4 | `uint xe_texture_load_guest_offset` | yes |
| 8 | 4 | `uint xe_texture_load_guest_pitch_aligned` | yes |
| 12 | 4 | `uint xe_texture_load_guest_z_stride_block_rows_aligned` | yes |
| 16 | 12 | `uint3 xe_texture_load_size_blocks` | yes |
| 28 | 4 | `uint xe_texture_load_host_offset` | yes |
| 32 | 4 | `uint xe_texture_load_host_pitch` | yes |
| 36 | 4 | `uint xe_texture_load_height_texels` | no |

### `texture_load_ctx1_cs`, `texture_load_dxn_rg8_cs`, `texture_load_dxt1_rgba8_cs`, `texture_load_dxt3_rgba8_cs`, `texture_load_dxt3a_cs`, `texture_load_dxt3aas1111_bgra4_cs`, `texture_load_dxt5_rgba8_cs`, `texture_load_dxt5a_r8_cs`

Stage: cs_5_0. Opt-in, unverified source (see above).

| Slot | Name | Type | Format | Dim | Count |
|---|---|---|---|---|---|
| `t0` | `xe_texture_load_source` | texture | uint4 | buf | 1 |
| `u0` | `xe_texture_load_dest` | UAV | uint4 | buf | 1 |
| `cb0` | `push_consts_xe` | cbuffer | NA | NA | 1 |

cbuffer `push_consts_xe` (`cb0`): reflected size 48 bytes, code reads the first 48 bytes (`dcl_constantbuffer`).

| Offset | Size | Member | Used |
|---|---|---|---|
| 0 | 4 | `uint xe_texture_load_is_tiled_3d_endian_scale` | yes |
| 4 | 4 | `uint xe_texture_load_guest_offset` | yes |
| 8 | 4 | `uint xe_texture_load_guest_pitch_aligned` | yes |
| 12 | 4 | `uint xe_texture_load_guest_z_stride_block_rows_aligned` | yes |
| 16 | 12 | `uint3 xe_texture_load_size_blocks` | yes |
| 28 | 4 | `uint xe_texture_load_host_offset` | yes |
| 32 | 4 | `uint xe_texture_load_host_pitch` | yes |
| 36 | 4 | `uint xe_texture_load_height_texels` | yes |
