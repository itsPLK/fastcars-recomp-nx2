#!/bin/sh
# fastcars-recomp-nx2: the whole build, inside the builder container (switch2/builder/Dockerfile).
# Started by fastcars.py (installer/core/build.py); everything happens in the mounted work folder:
#   /work/src         a copy of the installer's own source (COMMIT: its commit), plus the upstream
#                     checkouts this script adds
#   /work/data        the game files from the player's discs (hash-checked)
#   /work/codegen     the recompiled game code (C++ made from the player's default.xex)
#   /work/build       build trees
#   /work/out/switch/fastcars/   the result: fastcars.nro + args.txt (data/ is copied by the installer)
#
#   build.sh build DISC1 DISC2 TU     all steps (each skipped when already done)
#   build.sh shaders IN OUT           shader_cache microcode -> SPIR-V (translate_shaders.sh)
set -eu
WORK=/work
SRC=$WORK/src
JOBS=${JOBS:-$(nproc)}
CC_HOST="-DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# Upstream trees at their pins plus this repository's patches (patches/).
deps() {
  stamp=$WORK/build/deps.stamp
  P=$SRC/patches
  want=$(cat "$P"/rexglue-sdk-fastcars-linux.patch "$P"/rexglue-sdk-fastcars-switch2.patch \
             "$P"/glslang-fastcars-switch2.patch "$P"/plume-fastcars.patch "$P"/plume-fastcars-switch2.patch \
             "$P"/xenosrecomp-fastcars.patch | sha256sum | cut -c1-16)
  [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && return 0
  say "Fetching the SDK, plume and XenosRecomp and applying the patches"
  rm -rf "$SRC/rexglue-sdk" "$SRC/thirdparty/plume" "$SRC/thirdparty/XenosRecomp"
  git clone -q https://github.com/rexglue/rexglue-sdk "$SRC/rexglue-sdk"
  git -C "$SRC/rexglue-sdk" checkout -q f5337cd
  git -C "$SRC/rexglue-sdk" submodule update -q --init --recursive
  git -C "$SRC/rexglue-sdk" apply "$P/rexglue-sdk-fastcars-linux.patch"
  git -C "$SRC/rexglue-sdk" apply "$P/rexglue-sdk-fastcars-switch2.patch"
  git -C "$SRC/rexglue-sdk/thirdparty/glslang" apply "$P/glslang-fastcars-switch2.patch"
  mkdir -p "$SRC/thirdparty"
  git clone -q https://github.com/renderbag/plume "$SRC/thirdparty/plume"
  git -C "$SRC/thirdparty/plume" checkout -q 0ce184b
  git -C "$SRC/thirdparty/plume" submodule update -q --init --recursive
  git -C "$SRC/thirdparty/plume" apply "$P/plume-fastcars.patch"
  git -C "$SRC/thirdparty/plume" apply "$P/plume-fastcars-switch2.patch"
  git clone -q https://github.com/hedge-dev/XenosRecomp "$SRC/thirdparty/XenosRecomp"
  git -C "$SRC/thirdparty/XenosRecomp" checkout -q 990d03b
  git -C "$SRC/thirdparty/XenosRecomp" submodule update -q --init --recursive
  git -C "$SRC/thirdparty/XenosRecomp" apply "$P/xenosrecomp-fastcars.patch"
  rm -rf "$WORK/build/host" "$WORK/build/game"   # rebuilt against the new trees
  mkdir -p "$WORK/build" && echo "$want" > "$stamp"
}

# Linux tools: the data install, the codegen (rexglue) and XenosRecomp.
host_tools() {
  T=$WORK/build/host
  if [ ! -x "$T/data_install/data_install" ]; then
    say "Building the data installer"
    cmake -S "$SRC/switch2/tools/data_install" -B "$T/data_install" -G Ninja -DCMAKE_BUILD_TYPE=Release $CC_HOST >/dev/null
    cmake --build "$T/data_install"
  fi
  if [ ! -x "$T/sdk/rexglue" ]; then
    say "Building the recompiler (rexglue)"
    # (the SDK's linux-amd64 preset flags; X11/Wayland headers are only for its configure)
    cmake -S "$SRC/rexglue-sdk" -B "$T/sdk-build" -G Ninja -DCMAKE_BUILD_TYPE=Release $CC_HOST \
          -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_STANDARD=23 \
          -DREXGLUE_BUILD_TESTS=OFF >/dev/null
    cmake --build "$T/sdk-build" --target rexglue -- -j"$JOBS"
    # (the SDK writes its binaries to rexglue-sdk/out/<preset>)
    REXGLUE=$(find "$SRC/rexglue-sdk/out" "$T/sdk-build" -name rexglue -type f -perm -u+x 2>/dev/null | head -1)
    [ -n "$REXGLUE" ] || die "rexglue was not built"
    mkdir -p "$T/sdk" && cp "$REXGLUE" "$T/sdk/rexglue"
  fi
  if [ ! -x "$T/xenosrecomp/XenosRecomp/XenosRecomp" ]; then
    say "Building XenosRecomp"
    cmake -S "$SRC/thirdparty/XenosRecomp" -B "$T/xenosrecomp" -G Ninja -DCMAKE_BUILD_TYPE=Release $CC_HOST >/dev/null
    cmake --build "$T/xenosrecomp" -- -j"$JOBS"
  fi
}

data_install() {
  if [ -f "$WORK/data/data_installed.txt" ] && [ -f "$WORK/data/game/default.xex" ]; then return 0; fi
  [ $# -eq 3 ] || die "the discs and the title update are needed for the first build"
  say "Copying and checking the game files from your discs (about 16 GB)"
  "$WORK/build/host/data_install/data_install" "$WORK" "$SRC/switch2/data/expected.txt" "$1" "$2" "$3"
}

# FASTCARS_FAST_CODEGEN=0 builds the plain codegen (registers in PPCContext, every access volatile)
# instead of fastcars/codegen_fast.toml (registers as locals, memory access classes).
codegen() {
  C=$WORK/codegen
  stamp=$C/stamp
  fast=${FASTCARS_FAST_CODEGEN:-1}
  want=$( (echo "fast=$fast"; cat "$SRC/fastcars/fastcars_manifest.toml" "$SRC/fastcars/codegen_fast.toml" \
    "$SRC/fastcars/codegen_share_registers.toml" "$SRC/tools/game_dll/codegen.partition.json" \
    "$WORK/build/host/sdk/rexglue") | sha256sum | cut -c1-16)
  [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && [ -f "$C/generated/default/fastcars_init.cpp" ] && return 0
  say "Recompiling the game executable to C++ (several minutes)"
  rm -rf "$C" && mkdir -p "$C/generated/default" "$C/assets"
  cp "$SRC/fastcars/fastcars_manifest.toml" "$C/"
  if [ "$fast" = 1 ]; then
    cp "$SRC/fastcars/codegen_fast.toml" "$SRC/fastcars/codegen_share_registers.toml" "$C/"
    sed -i 's/^includes = \[\]/includes = ["codegen_fast.toml"]/' "$C/fastcars_manifest.toml"
    grep -q '^includes = \["codegen_fast.toml"\]' "$C/fastcars_manifest.toml" || die "codegen: manifest has no empty includes line"
  fi
  cp "$SRC/tools/game_dll/codegen.partition.json" "$C/generated/default/"
  cp "$WORK/data/game/default.xex" "$WORK/data/game/default.xexp" "$C/assets/"
  (cd "$C" && "$WORK/build/host/sdk/rexglue" --log-level warn codegen fastcars_manifest.toml)
  echo "$want" > "$stamp"
}

game() {
  B=$WORK/build/game
  mkdir -p "$B"
  say "Building the renderer's shaders"
  python3 "$SRC/switch2/game/native_shaders.py" "$B/native_shaders" >/dev/null
  say "Building fastcars.nro (the first build takes a while)"
  cmake -S "$SRC/switch2/game" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$SRC/switch2/cmake/switch2-clang.cmake" \
        -DFASTCARS_GENERATED_DIR="$WORK/codegen/generated/default" \
        -DFASTCARS_NATIVE_SHADER_DIR="$B/native_shaders" -DFASTCARS_NXEART="$WORK/data/game/nxeart" -Wno-dev >/dev/null
  cmake --build "$B" -- -j"$JOBS"
  O=$WORK/out/switch/fastcars
  mkdir -p "$O"
  cp "$B/fastcars.nro" "$O/fastcars.nro"
  [ -f "$O/args.txt" ] || cp "$SRC/switch2/game/args.txt" "$O/args.txt"
  say "Done: $O/fastcars.nro"
}

case "${1:-}" in
  build)
    shift
    [ -f "$SRC/switch2/builder/build.sh" ] || die "no source in $SRC"
    deps
    host_tools
    data_install "$@"
    codegen
    game
    ;;
  shaders)
    shift
    deps
    host_tools
    bash "$SRC/switch2/builder/translate_shaders.sh" "$WORK/build/host/xenosrecomp/XenosRecomp/XenosRecomp" "$@"
    ;;
  *)
    die "usage: build.sh build DISC1 DISC2 TU | build.sh shaders IN OUT"
    ;;
esac
