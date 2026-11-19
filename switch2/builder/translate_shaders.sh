#!/bin/bash
# Game shaders for Switch 2: the microcode the console saved (data/shader_cache/*.bin, every shader
# it met without SPIR-V) -> XenosRecomp (HLSL) -> constants as uniform buffers
# (switch2/tools/ubo_constants.py) -> dxc (SPIR-V, the runtime's arguments: shader_compiler.cpp).
# Shaders already translated in OUT are skipped, so a later fetch only adds the new ones.
#   translate_shaders.sh XENOSRECOMP IN OUT [JOBS]
set -eu
XR=$1 IN=$2 OUT=$3 JOBS=${4:-$(nproc)}
SRC=$(cd "$(dirname "$0")/../.." && pwd)
XRDIR=$SRC/thirdparty/XenosRecomp
COMMON=$XRDIR/XenosRecomp/shader_common.h
DXC=$XRDIR/thirdparty/dxc-bin/bin/x64/dxc-linux
export LD_LIBRARY_PATH=$XRDIR/thirdparty/dxc-bin/lib/x64
UBO=$SRC/switch2/tools/ubo_constants.py
WORK=$(mktemp -d)
mkdir -p "$OUT"

one() {
  bin=$1; name=$(basename "$bin" .bin); out=$OUT/$name.spv
  [ -s "$out" ] && return 0
  hlsl=$WORK/$name.hlsl
  if ! "$XR" "$bin" "$hlsl" "$COMMON" >/dev/null 2>&1; then echo "skip $name (XenosRecomp failed)"; return 0; fi
  # float2 shared-constant loads as two float loads, which ubo_constants.py maps.
  sed -i 's/vk::RawBufferLoad<float2>(\(g_PushConstants\.SharedConstants + \)\([0-9]*\))/float2(vk::RawBufferLoad<float>(\1\2), vk::RawBufferLoad<float>(\1\2 + 4))/' "$hlsl"
  if ! python3 -I "$UBO" "$hlsl" >/dev/null 2>&1; then echo "skip $name (constant rewrite failed)"; rm -f "$hlsl"; return 0; fi
  case $name in ps_*) prof=ps_6_0; extra=;; *) prof=vs_6_0; extra=-fvk-invert-y;; esac
  if ! "$DXC" -T $prof -HV 2021 -all-resources-bound -spirv -fvk-use-dx-layout -Wno-ignored-attributes \
       -Qstrip_debug $extra -Fo "$out.tmp" "$hlsl" >/dev/null 2>&1; then
    echo "skip $name (dxc failed)"; rm -f "$hlsl" "$out.tmp"; return 0
  fi
  mv "$out.tmp" "$out"
  # Vertex shaders that fetch by a computed index read the raw streams (the renderer checks this marker).
  case $name in vs_*) grep -q '= vfetchIndirect(' "$hlsl" && : > "$OUT/$name.indirect";; esac
  # The tile-classification variant (fastcars/src/native/shader_compiler.cpp, kVariants "_tiles2").
  if [ "$name" = ps_D258EBE32001470C ]; then
    sed -e 's/r0.y = select(r0.y > 0.0, c255.x, c255.y);/r0.y = c255.x;/' \
        -e 's/oC0.z = select(r0.w > 0.0, r1.x, c255.z);/oC0.z = select(r0.w > 0.0, max(r1.x, 0.0625), c255.z);/' \
        "$hlsl" > "$WORK/${name}_tiles2.hlsl"
    "$DXC" -T ps_6_0 -HV 2021 -all-resources-bound -spirv -fvk-use-dx-layout -Wno-ignored-attributes \
      -Qstrip_debug -Fo "$OUT/${name}_tiles2.spv" "$WORK/${name}_tiles2.hlsl" >/dev/null 2>&1 ||
      echo "skip ${name}_tiles2 (dxc failed)"
  fi
  rm -f "$hlsl"
}
export -f one
export XR COMMON DXC UBO OUT WORK LD_LIBRARY_PATH
before=$(ls "$OUT" | grep -c '\.spv$' || true)
ls "$IN"/*.bin 2>/dev/null | xargs -r -P "$JOBS" -I{} bash -c 'one "$@"' _ {}
after=$(ls "$OUT" | grep -c '\.spv$' || true)
rm -rf "$WORK"
echo "shaders: $((after - before)) new, $after in total"
