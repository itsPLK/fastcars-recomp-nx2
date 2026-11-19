#!/bin/sh
# Desktop A/B runs for Switch 2 performance work (docs/SWITCH2_PERF_PLAN.md): runs the macOS
# install headless with a given host build (fastcars_fast + SDK dylibs) and game library, scripted pad
# (tools/native/input_run_around.txt: A every second, run forward), and reports CPU time,
# presented frames and the renderer's per-300-frame lines.
#   mac_bench.sh SECONDS NAME=DYLIB [NAME=DYLIB ...]
#   HOST      folder with fastcars_fast, librexruntime.dylib, ... (default ../fastcars-mac/perf/host)
#   FASTCARS_HOME macOS game folder with data/ (default ../fastcars-mac/home)
#   EXTRA     extra runtime flags for every run
#   SHOTS=1   native-renderer screenshots every 20 s into <run>/shots (PPM)
#   SAMPLE=N  macOS `sample` of the process for the last N seconds into <run>/sample.txt
# Each variant runs from its own folder next to its dylib (symlinks to the host and the install,
# a private copy of data/user so saves never touch the install). Names that repeat alternate.
R="$(cd "$(dirname "$0")/../.." && pwd)"
H=${FASTCARS_HOME:-$R/../fastcars-mac/home}
HOST=$(cd "${HOST:-$R/../fastcars-mac/perf/host}" && pwd)
D=$H/data
SECS=$1
shift
for spec in "$@"; do
  name=${spec%%=*}
  lib=$(cd "$(dirname "${spec#*=}")" && pwd)/$(basename "${spec#*=}")
  run=$(dirname "$lib")/run-$name
  mkdir -p "$run"
  for f in "$HOST"/*.dylib "$HOST"/vulkan "$HOST"/tools "$H"/shader_cache; do
    ln -sfn "$f" "$run/$(basename "$f")"
  done
  # copied, not linked: @executable_path (the rpath) would resolve to the host folder
  rm -f "$run/fastcars_fast" "$run/fastcars_game.dylib"
  cp "$HOST/fastcars_fast" "$run/fastcars_fast"
  cp "$lib" "$run/fastcars_game.dylib"
  [ -d "$run/user" ] || cp -R "$D/user" "$run/user"
  log=$run/bench.log
  rm -f "$log"
  shots=
  if [ "${SHOTS:-0}" = 1 ]; then rm -rf "$run/shots"; mkdir -p "$run/shots"; shots=$run/shots; fi
  cd "$run" || exit 1
  FASTCARS_NATIVE_SHOT_DIR=$shots FASTCARS_SHOT_SECS=20 \
  ./fastcars_fast --game_data_root="$D/game" --user_data_root="$run/user" --update_data_root="$D/update" \
    --fastcars_native_spirv_dir="$D/spirv" --gpu_plugin=xenos --headless=true --log_level=info \
    --log_max_file_size_mb=256 --log_max_files=2 --fullscreen=false --mnk_mode=false \
    --fastcars_native=on --fastcars_fps_limit=0 --gpu_skip_rendering=true --gpu_skip_rendering_lag_ms=16 \
    --vsync=false --fastcars_native_vsync=false --fastcars_native_hidden=true --audio_mute=true \
    --input_script="$R/tools/native/input_run_around.txt" \
    --log_file="$log" $EXTRA > "$run/stdout.txt" 2>&1 &
  pid=$!
  if [ "${SAMPLE:-0}" -gt 0 ]; then
    sleep $((SECS - SAMPLE - 2))
    sample $pid "$SAMPLE" -file "$run/sample.txt" > /dev/null 2>&1
    sleep 2
  else
    sleep "$SECS"
  fi
  cpu=$(ps -p $pid -o time= | tr -d ' ')
  # per-thread CPU (the busiest: the game's main thread, render thread, workers)
  ps -M -p $pid 2>/dev/null | awk 'NR>2 { split($(NF-1), a, ":"); split($NF, b, ":");
    printf "%.0f\n", a[1] * 60 + a[2] + b[1] * 60 + b[2] }' | sort -nr | head -8 > "$run/threads.txt"
  alive=yes
  kill -0 $pid 2>/dev/null || alive="NO (exited early)"
  kill $pid 2>/dev/null; sleep 3; kill -9 $pid 2>/dev/null
  last=$(grep "presented [0-9]* frames" "$log" | tail -1 | sed 's/.*presented \([0-9]*\) frames.*/\1/')
  echo "$name: cpu $cpu in ${SECS}s, frames presented ${last:-0}, alive at end: $alive, log $log"
  echo "  busiest threads (CPU s): $(tr '\n' ' ' < "$run/threads.txt")"
  cd "$R" || exit 1
done
