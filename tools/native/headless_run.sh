#!/bin/sh
# headless_run.sh <name> [flags]: benchmark run without a window (macOS): screenshots every
# SHOT_SECS (default 30) to /Volumes/Untitled/fastcars-captures/<name>, log logs/<name>.log, scripted
# pad from logs/<name>.pad once that file exists (see --input_script). Returns once the open world
# renders (or the game exited). PASS_STATS=1: FASTCARS_NATIVE_PASS_STATS.
R="$(cd "$(dirname "$0")/../.." && pwd)"
N=$1; shift
cd "$R" || exit 1
[ -n "$PASS_STATS" ] && export FASTCARS_NATIVE_PASS_STATS=1
pkill -x fastcars; sleep 1
rm -f logs/$N.pad; mkdir -p /Volumes/Untitled/fastcars-captures/$N
FASTCARS_NATIVE_SHOT_DIR=/Volumes/Untitled/fastcars-captures/$N FASTCARS_SHOT_SECS=${SHOT_SECS:-30} FASTCARS_IGNORE_SETTINGS=1 \
  FASTCARS_NATIVE=on nohup tools/mac/play_mac.sh $N --fastcars_native_hidden=true --fullscreen=false \
  --input_script="$R/logs/$N.pad" "$@" > logs/$N.stdout 2>&1 &
sleep 5
until grep -qE "per frame 1[2-9][0-9] render passes" logs/$N.log 2>/dev/null || ! pgrep -x fastcars >/dev/null; do sleep 2; done
pgrep -x fastcars >/dev/null && echo "world loaded" || { echo "EXITED"; tail -3 logs/$N.log; }
