#!/bin/sh
# Launch the recompiled game (TU27) on macOS (arm64), older desktop build layout (build/fastcars).
# Usage: tools/mac/play_mac.sh [logname] [extra runtime flags...]
# Game data: FASTCARS_DATA (default ~/fastcars-ssd if present, else ~/fastcars-data; must be a case-insensitive
# volume, Xbox paths ignore case). game/ and userdata/ are read; the internal SSD loads far faster than USB.
# FASTCARS_NATIVE=off (default here until the native renderer is ported) | on | shadow;
# FASTCARS_FPS=0 (uncapped) | 30 | N; FASTCARS_LOG_LEVEL=debug for verbose logs. Keyboard plays as a pad
# (--mnk_mode: Space = A, Backspace = B, Return = Start, WASD = left stick, arrows = right stick).
R="$(cd "$(dirname "$0")/../.." && pwd)"  # the repository
if [ -z "$FASTCARS_DATA" ] && [ -d "$HOME/fastcars-ssd/game" ]; then FASTCARS_DATA="$HOME/fastcars-ssd"; fi
E="${FASTCARS_DATA:-$HOME/fastcars-data}"
BUILD="${FASTCARS_BUILD:-$R/build/fastcars}"
name=${1:-play}; [ $# -gt 0 ] && shift
mkdir -p "$R/logs"
cd "$BUILD" || exit 1
mode=${FASTCARS_NATIVE:-off}
set -- --fastcars_native=$mode --fastcars_fps_limit=${FASTCARS_FPS:-0} --audio_device_frames=256 --mnk_mode=true "$@"
[ "$mode" = on ] && set -- --gpu_skip_rendering=true --gpu_skip_rendering_lag_ms=${FASTCARS_CP_LAG:-6} --vsync=false --fastcars_native_vsync=false "$@"
exec ./fastcars \
  --game_data_root="$E/game/disc2_files" \
  --user_data_root="$E/userdata" \
  --update_data_root="$E/game/update" \
  --gpu_plugin=xenos --headless=true --log_level=${FASTCARS_LOG_LEVEL:-info} \
  --log_max_file_size_mb=1024 --log_max_files=2 \
  --log_file="$R/logs/$name.log" "$@"
