#!/bin/bash
# Switch 2: the game's SPIR-V with shader constants read from uniform buffers (descriptor set 4,
# switch2/tools/ubo_constants.py) instead of buffer-device-address loads. NVIDIA keeps uniform
# buffers in its constant banks; the device-address loads were plain memory loads per invocation.
# The backend binds set 4 when --fastcars_native_ubo_constants is on (Switch 2 default).
#
# Input: the macOS game folder's shader cache (XenosRecomp HLSL next to each microcode .bin, made by
# the Mac app) and its SPIR-V dir (for the .indirect markers and as the fallback for a shader the
# rewrite cannot map). Output: a spirv dir for the SD card (sdmc:/switch/fastcars/data/spirv_ubo).
# Compiles with the release's dxc and exactly the runtime's arguments (shader_compiler.cpp).
#
# usage: switch2/tools/make_spirv_ubo.sh [OUT] [JOBS]
#   (defaults: ../fastcars-mac/build/spirv_ubo, 8; game folder ../fastcars-mac/home, override with GAME=)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GAME=${GAME:-$ROOT/../fastcars-mac/home}
REL=${REL:-$ROOT/../fastcars-mac/release/fastcars-recomp-macos-arm64/FASTCARS.app/Contents/Resources/fastcars}
OUT=${1:-$ROOT/../fastcars-mac/build/spirv_ubo}
JOBS=${2:-8}
CACHE=$GAME/data/shader_cache
SPIRV=$GAME/data/spirv
DXC=$REL/tools/shaders/dxc
UBO=$ROOT/switch2/tools/ubo_constants.py
WORK=$OUT.work
[ -x "$DXC" ] || { echo "no dxc at $DXC"; exit 1; }
mkdir -p "$OUT" "$WORK"
cp "$SPIRV"/*.indirect "$OUT"/ 2>/dev/null || true

one() {
  hlsl=$1; name=$(basename "$hlsl" .hlsl); out=$OUT/$name.spv
  [ -s "$out" ] && [ "$out" -nt "$hlsl" ] && [ "$out" -nt "$UBO" ] && return 0
  w=$WORK/$name.hlsl
  # float2 shared-constant loads as two float loads, which ubo_constants.py maps.
  sed -e 's/vk::RawBufferLoad<float2>(\(g_PushConstants\.SharedConstants + \)\([0-9]*\))/float2(vk::RawBufferLoad<float>(\1\2), vk::RawBufferLoad<float>(\1\2 + 4))/' \
      "$hlsl" > "$w"
  if ! python3 -I "$UBO" "$w" >/dev/null 2>"$WORK/$name.err"; then
    echo "ubo fail $name (kept the device-address SPIR-V)"
    cp "$SPIRV/$name.spv" "$out" 2>/dev/null || true
    rm -f "$w"; return 0
  fi
  case $name in ps_*) prof=ps_6_0; extra=;; *) prof=vs_6_0; extra=-fvk-invert-y;; esac
  if ! DYLD_LIBRARY_PATH="$(dirname "$DXC")/lib" "$DXC" -T $prof -HV 2021 -all-resources-bound -spirv \
      -fvk-use-dx-layout -Wno-ignored-attributes -Qstrip_debug $extra -Fo "$out" "$w" >"$WORK/$name.err" 2>&1; then
    echo "dxc fail $name (kept the device-address SPIR-V)"
    cp "$SPIRV/$name.spv" "$out" 2>/dev/null || true
    rm -f "$w"; return 0
  fi
  # (The tile-classification variant ps_D258EBE32001470C_tiles2 is in the cache as its own HLSL.)
  rm -f "$w" "$WORK/$name.err"
}
export -f one; export OUT WORK DXC UBO SPIRV
ls "$CACHE"/*.hlsl | xargs -P "$JOBS" -I{} bash -c 'one {}'
echo "spirv_ubo: $(ls "$OUT"/*.spv | wc -l | tr -d ' ') .spv, $(ls "$OUT"/*.indirect 2>/dev/null | wc -l | tr -d ' ') .indirect in $OUT"
