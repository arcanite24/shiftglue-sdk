#!/usr/bin/env bash
# Regenerates the Shader Model 5.0 (Direct3D 11) UI shader headers in
# bytecode/d3d11_5_0/ from the upstream Xenia XeSL sources.
#
# Usage: compile-d3d11.sh [xenia-ui-shaders-dir]
#
# The source directory is xenia-canary's src/xenia/ui/shaders (with its
# third_party/FidelityFX-CAS and FidelityFX-FSR four levels up, which the
# FidelityFX shaders include by relative path). Without an argument,
# XENIA_UI_SHADERS is used, then references/upstream/xenia-canary in a parent
# directory of this script or in a sibling horizon1-recomp checkout.
#
# FXC is taken from FXC_PATH, else the newest Windows 10 SDK fxc.exe.
#
# The flags match the d3d12_5_1 headers (fxc default optimization level 1 and
# /Gfp; compiling the same sources as ps_5_1/vs_5_1 with them reproduces the
# d3d12_5_1 bytecode byte for byte). The only source difference is
# d3d11_sm50.xesli, which removes the register spaces SM 5.0 lacks.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
out_dir="$script_dir/bytecode/d3d11_5_0"

to_win() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi
}

src_dir="${1:-${XENIA_UI_SHADERS:-}}"
if [[ -z "$src_dir" ]]; then
  d="$script_dir"
  while [[ "$d" != "/" && -n "$d" ]]; do
    for candidate in \
        "$d/references/upstream/xenia-canary/src/xenia/ui/shaders" \
        "$d/../horizon1-recomp/references/upstream/xenia-canary/src/xenia/ui/shaders"; do
      if [[ -f "$candidate/xesl.xesli" ]]; then
        src_dir="$candidate"
        break 2
      fi
    done
    d="$(dirname "$d")"
  done
fi
if [[ -z "$src_dir" || ! -f "$src_dir/xesl.xesli" ]]; then
  echo "error: xenia ui/shaders directory not found; pass it as the argument" >&2
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

shaders=(
  guest_output_bilinear.ps
  guest_output_bilinear_dither.ps
  guest_output_ffx_cas_resample.ps
  guest_output_ffx_cas_resample_dither.ps
  guest_output_ffx_cas_sharpen.ps
  guest_output_ffx_cas_sharpen_dither.ps
  guest_output_ffx_fsr_easu.ps
  guest_output_ffx_fsr_rcas.ps
  guest_output_ffx_fsr_rcas_dither.ps
  guest_output_triangle_strip_rect.vs
  immediate.ps
  immediate.vs
)

mkdir -p "$out_dir"
stub_dir="$(mktemp -d)"
trap 'rm -rf "$stub_dir"' EXIT

src_win="$(to_win "$src_dir")"
wrapper_win="$(to_win "$script_dir")"
for shader in "${shaders[@]}"; do
  id="${shader//./_}"
  stage="${id: -2}"
  stub="$stub_dir/$id.hlsl"
  printf '#include "d3d11_sm50.xesli"\n#include "%s/%s.xesl"\n' \
      "$src_win" "$shader" > "$stub"
  echo "$id (${stage}_5_0)"
  MSYS_NO_PATHCONV=1 "$fxc" /nologo /D SHADING_LANGUAGE_HLSL_XE=1 \
      /I "$wrapper_win" /I "$src_win" /T "${stage}_5_0" /Vn "$id" /Gfp \
      /Fh "$(to_win "$out_dir/$id.h")" "$(to_win "$stub")" >/dev/null
done

# fxc writes CRLF; the checked-in headers use LF like the d3d12_5_1 ones.
sed -i 's/\r$//' "$out_dir"/*.h
