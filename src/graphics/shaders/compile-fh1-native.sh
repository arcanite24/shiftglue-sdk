#!/usr/bin/env bash
# Compiles the FH1 native executor shaders into bytecode/d3d12_5_1 headers.
set -euo pipefail
cd "$(dirname "$0")"
FXC="${FXC:-/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe}"
compile() {  # profile file name defines...
  local profile=$1 file=$2 name=$3
  shift 3
  local args=()
  local define
  for define in "$@"; do args+=(//D "$define"); done
  "$FXC" //nologo //T "$profile" //E main //O3 "${args[@]}" //Vn "$name" \
    //Fh "bytecode/d3d12_5_1/$name.h" "$file" > /dev/null
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
echo compiled
