#!/usr/bin/env bash
# Regenerates the Shader Model 5.0 (Direct3D 11, feature level 11_0) graphics
# shader headers in bytecode/d3d11_5_0/ from source. File and variable names
# match bytecode/d3d12_5_1/, so a backend can include either.
#
# Usage: compile-d3d11.sh [xenia-gpu-shaders-dir]
#
# Sources:
# - Our HLSL in this directory (fh1_*), with the defines and flags of
#   compile-fh1-native.sh.
# - Upstream Xenia for the rest: xenia-canary's src/xenia/gpu/shaders (its
#   ../../ui/shaders/xesl.xesli is included by relative path). Without an
#   argument, XENIA_GPU_SHADERS is used, then references/upstream/xenia-canary
#   in a parent directory of this script or in a sibling horizon1-recomp
#   checkout. Verified against xenia-canary 22d91c8; the checkout is not
#   pinned, so re-check the d3d12_5_1 reproduction after updating it.
# - d3d11_sm50.xesli includes ../../ui/shaders/d3d11_sm50.xesli, the binding
#   header the UI shaders' compile-d3d11.sh uses.
#
# Only the shaders the D3D12 backend includes are built. Compiling the same
# sources and flags as *_5_1 reproduces the d3d12_5_1 instruction streams:
# - fh1_native_*, fh1_scaled_32bpp_2x: /O3 (byte for byte).
# - fh1_reflection_cube_import: /O3 /WX (byte for byte).
# - Xenia shaders: fxc default optimization level and /Gfp, reflection kept.
#   Upstream's own build (/O3 and /Qstrip_*) does not reproduce our headers.
#   The hull shaders and tessellation vertex shaders differ only in unused
#   members of xe_system_cbuffer (see BINDINGS.md).
# - texture_load_*: current upstream does NOT reproduce our d3d12_5_1 code
#   (different tiled address code and byte address buffers instead of typed
#   buffers). They are built only with D3D11_UNVERIFIED_TEXTURE_LOAD=1, from
#   current upstream through d3d11_texture_load_typed.xesli, which restores
#   the typed buffer bindings (the reflection then matches d3d12_5_1).
#
# XeSL sources go through a stub that includes d3d11_sm50.xesli first, which
# drops the register spaces SM 5.0 lacks. Every graphics shader uses space0,
# so no register is remapped.
#
# FXC is taken from FXC_PATH, else the newest Windows 10 SDK fxc.exe.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
out_dir="$script_dir/bytecode/d3d11_5_0"

to_win() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi
}

src_dir="${1:-${XENIA_GPU_SHADERS:-}}"
if [[ -z "$src_dir" ]]; then
  d="$script_dir"
  while [[ "$d" != "/" && -n "$d" ]]; do
    for candidate in \
        "$d/references/upstream/xenia-canary/src/xenia/gpu/shaders" \
        "$d/../horizon1-recomp/references/upstream/xenia-canary/src/xenia/gpu/shaders"; do
      if [[ -f "$candidate/xenos_draw.hlsli" ]]; then
        src_dir="$candidate"
        break 2
      fi
    done
    d="$(dirname "$d")"
  done
fi
if [[ -z "$src_dir" || ! -f "$src_dir/xenos_draw.hlsli" ]]; then
  echo "error: xenia gpu/shaders directory not found; pass it as the argument" >&2
  exit 1
fi

fxc="${FXC_PATH:-}"
if [[ -z "$fxc" ]]; then
  kits="/c/Program Files (x86)/Windows Kits/10/bin"
  fxc="$(ls -d "$kits"/10.*/x64/fxc.exe 2>/dev/null | sort -V | tail -n 1 || true)"
fi
if [[ -z "$fxc" || ! -f "$fxc" ]]; then
  echo "error: fxc.exe not found; set FXC_PATH" >&2
  exit 1
fi

mkdir -p "$out_dir"
stub_dir="$(mktemp -d)"
trap 'rm -rf "$stub_dir"' EXIT

src_win="$(to_win "$src_dir")"
here_win="$(to_win "$script_dir")"

run_fxc() {  # profile name source flags-and-defines...
  local profile=$1 name=$2 source=$3
  shift 3
  echo "$name ($profile)"
  MSYS_NO_PATHCONV=1 "$fxc" /nologo /T "$profile" /E main "$@" /Vn "$name" \
      /Fh "$(to_win "$out_dir/$name.h")" "$source" >/dev/null
}

# Upstream Xenia XeSL shader through the SM 5.0 binding stub.
xenia_xesl() {  # shader.stage [wrapper]
  local shader=$1 wrapper=${2:-d3d11_sm50.xesli}
  local name="${shader//./_}"
  local stub="$stub_dir/$name.hlsl"
  printf '#include "%s"\n#include "%s/%s.xesl"\n' "$wrapper" "$src_win" \
      "$shader" > "$stub"
  run_fxc "${name: -2}_5_0" "$name" "$(to_win "$stub")" \
      /D SHADING_LANGUAGE_HLSL_XE=1 /I "$here_win" /I "$src_win" \
      /I "$src_win/../../ui/shaders" /Gfp
}

# Upstream Xenia plain HLSL shader (explicit space-free registers).
xenia_hlsl() {  # shader.stage
  local shader=$1
  local name="${shader//./_}"
  run_fxc "${name: -2}_5_0" "$name" "$src_win/$shader.hlsl" \
      /D SHADING_LANGUAGE_HLSL_XE=1 /I "$src_win" /Gfp
}

# Our HLSL in this directory.
fh1() {  # profile file name extra-flags-and-defines...
  local profile=$1 file=$2 name=$3
  shift 3
  run_fxc "$profile" "$name" "$here_win/$file" /O3 "$@"
}

for shader in apply_gamma_pwl.cs apply_gamma_pwl_fxaa_luma.cs \
    apply_gamma_table.cs apply_gamma_table_fxaa_luma.cs fullscreen_cw.vs; do
  xenia_xesl "$shader"
done

for shader in fxaa.cs fxaa_extreme.cs float24_round.ps float24_truncate.ps \
    adaptive_quad.hs adaptive_triangle.hs \
    continuous_quad_1cp.hs continuous_quad_4cp.hs \
    continuous_triangle_1cp.hs continuous_triangle_3cp.hs \
    discrete_quad_1cp.hs discrete_quad_4cp.hs \
    discrete_triangle_1cp.hs discrete_triangle_3cp.hs \
    tessellation_adaptive.vs tessellation_indexed.vs; do
  xenia_hlsl "$shader"
done

if [[ ${D3D11_UNVERIFIED_TEXTURE_LOAD:-0} == 1 ]]; then
  echo "warning: texture_load_* from current upstream; not proven equal to" \
      "the d3d12_5_1 bytecode" >&2
  for format in 8bpb 16bpb 32bpb 64bpb 128bpb depth_float depth_unorm \
      r10g11b11_rgba16 r10g11b11_rgba16_snorm r11g11b10_rgba16 \
      r11g11b10_rgba16_snorm r4g4b4a4_b4g4r4a4 r5g5b5a1_b5g5r5a1 \
      r5g5b6_b5g6r5_swizzle_rbga r5g6b5_b5g6r5; do
    xenia_xesl "texture_load_$format.cs" d3d11_texture_load_typed.xesli
    xenia_xesl "texture_load_${format}_scaled.cs" d3d11_texture_load_typed.xesli
  done
  for format in bgrg8_rgb8 bgrg8_rgbg8 ctx1 dxn_rg8 dxt1_rgba8 dxt3_rgba8 \
      dxt3a dxt3aas1111_bgra4 dxt5_rgba8 dxt5a_r8 gbgr8_grgb8 gbgr8_rgb8; do
    xenia_xesl "texture_load_$format.cs" d3d11_texture_load_typed.xesli
  done
fi

# FH1 native executor: the variants compile-fh1-native.sh builds that the
# D3D12 backend uses (not the resolve_image ones).
for source_kind in color depth uint; do
  source_defines=()
  [[ $source_kind == depth ]] && source_defines+=(/D FH1_SOURCE_DEPTH=1)
  [[ $source_kind == uint ]] && source_defines+=(/D FH1_SOURCE_UINT=1)
  for source_msaa in "" _ms; do
    msaa_defines=("${source_defines[@]}")
    [[ -n $source_msaa ]] && msaa_defines+=(/D FH1_SOURCE_MSAA=1)
    fh1 cs_5_0 fh1_native_resolve_memory.cs.hlsl \
      "fh1_native_resolve_memory_${source_kind}${source_msaa}_cs" "${msaa_defines[@]}"
    fh1 cs_5_0 fh1_native_transfer_words.cs.hlsl \
      "fh1_native_transfer_words_${source_kind}${source_msaa}_cs" "${msaa_defines[@]}"
    dest_index=0
    for dest_kind in color depth stencil uint; do
      for dest_msaa in "" _dms; do
        dest_defines=("${msaa_defines[@]}" /D "FH1_DEST_KIND=$dest_index")
        [[ -n $dest_msaa ]] && dest_defines+=(/D FH1_DEST_MSAA=1)
        fh1 ps_5_0 fh1_native_transfer.ps.hlsl \
          "fh1_native_transfer_${dest_kind}${dest_msaa}_from_${source_kind}${source_msaa}_ps" \
          "${dest_defines[@]}"
      done
      dest_index=$((dest_index + 1))
    done
  done
done
kind=1
for dest_kind in depth stencil; do
  for dest_msaa in "" _dms; do
    defines=(/D "FH1_DEST_KIND=$kind")
    [[ -n $dest_msaa ]] && defines+=(/D FH1_DEST_MSAA=1)
    fh1 ps_5_0 fh1_native_transfer_from_words.ps.hlsl \
      "fh1_native_transfer_${dest_kind}${dest_msaa}_from_words_ps" "${defines[@]}"
  done
  kind=2
done

# Texture cache: scaled 32-bpp resolve buffer and reflection cube imports.
fh1 cs_5_0 fh1_scaled_32bpp_2x.cs.hlsl fh1_scaled_32bpp_2x_cs
fh1 cs_5_0 fh1_reflection_cube_import.cs.hlsl fh1_reflection_cube_import_cs /WX
# D3D11 texture cache only (no d3d12_5_1 counterpart): scratch buffer to
# texture copies through typed UINT UAVs.
fh1 cs_5_0 d3d11_texture_copy.cs.hlsl d3d11_texture_copy_2d_cs /WX
fh1 cs_5_0 d3d11_texture_copy.cs.hlsl d3d11_texture_copy_3d_cs /WX /D XE_TEXTURE_COPY_3D=1
fh1 cs_5_0 d3d11_swap_swizzle.cs.hlsl d3d11_swap_swizzle_cs /WX

# fxc writes CRLF; the checked-in headers use LF like the d3d12_5_1 ones.
sed -i 's/\r$//' "$out_dir"/*.h
echo compiled
