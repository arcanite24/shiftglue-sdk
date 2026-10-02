#!/usr/bin/env bash
# Compiles the FH1 native executor and texture cache shaders into
# bytecode/d3d12_5_1 headers and, from the same HLSL, into vulkan_spirv
# SPIR-V headers.
#
# SPIR-V comes from the vendored glslang's HLSL front end, so no DXC with
# SPIR-V code generation is needed. Build glslangValidator once from
# thirdparty/glslang with ENABLE_HLSL and ENABLE_GLSLANG_BINARIES and point
# GLSLANG at it; without GLSLANG only the DXBC headers are written.
#
# Vulkan: the constant buffers are push constants (FH1_SPIRV, see
# fh1_push_constants.hlsli), and resources are in descriptor set 0 by register
# class whatever their type (--hlsl-iomap): t<n> is binding 16 + n and u<n> is
# binding 32 + n.
set -euo pipefail
cd "$(dirname "$0")"
FXC="${FXC:-/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe}"
GLSLANG="${GLSLANG:-}"
compile_spirv() {  # profile file name defines...
  local profile=$1 file=$2 name=$3
  shift 3
  [[ -n $GLSLANG ]] || return 0
  local args=()
  local define
  for define in "$@"; do args+=("-D$define"); done
  local stage=frag
  [[ $profile == cs_* ]] && stage=comp
  "$GLSLANG" -D -V -S "$stage" -e main -DFH1_SPIRV=1 "${args[@]}" --hlsl-iomap \
    --shift-texture-binding 16 --shift-UAV-binding 32 \
    --vn "$name" -o "vulkan_spirv/$name.h" "$file" > /dev/null
}
compile() {  # profile file name defines...
  local profile=$1 file=$2 name=$3
  shift 3
  local args=()
  local define
  for define in "$@"; do args+=(//D "$define"); done
  "$FXC" //nologo //T "$profile" //E main //O3 "${args[@]}" //Vn "$name" \
    //Fh "bytecode/d3d12_5_1/$name.h" "$file" > /dev/null
  compile_spirv "$profile" "$file" "$name" "$@"
}
for source_kind in color depth uint; do
  source_defines=()
  [[ $source_kind == depth ]] && source_defines+=(FH1_SOURCE_DEPTH=1)
  [[ $source_kind == uint ]] && source_defines+=(FH1_SOURCE_UINT=1)
  for source_msaa in "" _ms; do
    msaa_defines=("${source_defines[@]}")
    [[ -n $source_msaa ]] && msaa_defines+=(FH1_SOURCE_MSAA=1)
    compile cs_5_1 fh1_native_resolve_memory.cs.hlsl \
      "fh1_native_resolve_memory_${source_kind}${source_msaa}_cs" "${msaa_defines[@]}"
    compile cs_5_1 fh1_native_transfer_words.cs.hlsl \
      "fh1_native_transfer_words_${source_kind}${source_msaa}_cs" "${msaa_defines[@]}"
    dest_index=0
    for dest_kind in color depth stencil uint; do
      for dest_msaa in "" _dms; do
        dest_defines=("${msaa_defines[@]}" "FH1_DEST_KIND=$dest_index")
        [[ -n $dest_msaa ]] && dest_defines+=(FH1_DEST_MSAA=1)
        compile ps_5_1 fh1_native_transfer.ps.hlsl \
          "fh1_native_transfer_${dest_kind}${dest_msaa}_from_${source_kind}${source_msaa}_ps" \
          "${dest_defines[@]}"
      done
      dest_index=$((dest_index + 1))
    done
  done
done
# Depth destinations: depth and stencil-bit passes from precomputed words.
kind=1
for dest_kind in depth stencil; do
  for dest_msaa in "" _dms; do
    defines=("FH1_DEST_KIND=$kind")
    [[ -n $dest_msaa ]] && defines+=(FH1_DEST_MSAA=1)
    compile ps_5_1 fh1_native_transfer_from_words.ps.hlsl \
      "fh1_native_transfer_${dest_kind}${dest_msaa}_from_words_ps" "${defines[@]}"
  done
  kind=2
done
# Depth and stencil in one pass straight from the source, for Vulkan devices
# with shader stencil export (FH1_DEST_KIND 4). glslang's HLSL front end has no
# SV_StencilRef (nor has Direct3D 12's shader model 5.1): the shader writes the
# value to an int target at location 7, which spirv_stencil_export.py turns
# into the FragStencilRefEXT built-in.
if [[ -n $GLSLANG ]]; then
  PYTHON="${PYTHON:-python3}"
  spirv_temp="$(mktemp -d)"
  for source_kind in color depth uint; do
    for source_msaa in "" _ms; do
      for dest_msaa in "" _dms; do
        defines=(-DFH1_SPIRV=1 -DFH1_DEST_KIND=4)
        [[ $source_kind == depth ]] && defines+=(-DFH1_SOURCE_DEPTH=1)
        [[ $source_kind == uint ]] && defines+=(-DFH1_SOURCE_UINT=1)
        [[ -n $source_msaa ]] && defines+=(-DFH1_SOURCE_MSAA=1)
        [[ -n $dest_msaa ]] && defines+=(-DFH1_DEST_MSAA=1)
        name="fh1_native_transfer_depth_stencil${dest_msaa}_from_${source_kind}${source_msaa}_ps"
        "$GLSLANG" -D -V -S frag -e main "${defines[@]}" --hlsl-iomap \
          --shift-texture-binding 16 --shift-UAV-binding 32 \
          -o "$spirv_temp/$name.spv" fh1_native_transfer.ps.hlsl > /dev/null
        "$PYTHON" spirv_stencil_export.py "$spirv_temp/$name.spv" "vulkan_spirv/$name.h" "$name"
      done
    done
  done
  rm -r "$spirv_temp"
fi
# Texture cache: the scaled 32-bpp resolve buffer and reflection cube imports.
# Their DXBC headers were built with other fxc flags and are kept as they are.
compile_spirv cs_5_1 fh1_scaled_32bpp_2x.cs.hlsl fh1_scaled_32bpp_2x_cs
compile_spirv cs_5_1 fh1_reflection_cube_import.cs.hlsl fh1_reflection_cube_import_cs
echo compiled
