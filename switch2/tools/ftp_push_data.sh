#!/bin/sh
# Copies the installed game data to the Switch 2's SD card over FTP (the console's FTP server, e.g.
# OunceShell's): sdmc:/switch/fastcars/data/{game,update,spirv,user}. Game-derived data: it only ever goes from
# the owner's own install to the owner's own console, never into git.
#   ftp_push_data.sh HOST:PORT [DATA_DIR]      DATA_DIR default ../fastcars-mac/home/data
# Files already on the card with the same size are skipped, so an interrupted copy resumes when run
# again. The Mac's shader_cache is not copied (the Switch builds its own). Keep the FTP app open on
# the console while it runs.
set -u
HOST=$1
R="$(cd "$(dirname "$0")/../.." && pwd)"
SRC=${2:-$R/../fastcars-mac/home/data}
URL=ftp://$HOST/switch/fastcars/data
cd "$SRC" || exit 1

remote_size() {
  curl -sI --max-time 20 "$URL/$1" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-length"{print $2}'
}

push() {  # one file, skipped when the card already has it at the same size
  f=$1
  local_size=$(stat -f %z "$f" 2>/dev/null || stat -c %s "$f")
  if [ "$(remote_size "$f")" = "$local_size" ]; then
    echo "skip  $f"
    return 0
  fi
  start=$(date +%s)
  if curl -sS --ftp-create-dirs --retry 3 -T "$f" "$URL/$f"; then
    secs=$(( $(date +%s) - start )); [ $secs -gt 0 ] || secs=1
    echo "ok    $f ($((local_size / 1048576)) MB, $((local_size / 1048576 / secs)) MB/s)"
  else
    echo "FAIL  $f"
    return 1
  fi
}

# FAT32 cards hold no file of 4 GiB or more. This game's largest file is 2.76 GB (the same for every
# player: same discs, same TU), but check anyway rather than fail halfway through a transfer.
big=$(find update game user -type f -size +4194303k 2>/dev/null)
if [ -n "$big" ]; then
  echo "These files are 4 GiB or larger and do not fit on a FAT32 SD card (exFAT is fine):"
  echo "$big"
  [ "${ALLOW_BIG:-}" = 1 ] || { echo "Set ALLOW_BIG=1 if the card is exFAT."; exit 1; }
fi

failures=0
for d in update game user; do
  find "$d" -type f | sort | while read -r f; do push "$f" || echo "$f" >> "$SRC/../ftp_push_failed.txt"; done
done
# spirv: thousands of small files in one connection (curl's {a,b,...} upload list).
if [ -d spirv ]; then
  have=$(curl -s --max-time 60 "$URL/spirv/" | wc -l | tr -d ' ')
  want=$(ls spirv | wc -l | tr -d ' ')
  if [ "$have" -ge "$want" ]; then
    echo "skip  spirv ($have files on the card)"
  else
    list=$(ls spirv | paste -sd, -)
    curl -sS --ftp-create-dirs --retry 3 -T "spirv/{$list}" "$URL/spirv/" && echo "ok    spirv ($want files)"
  fi
fi
[ -f "$SRC/../ftp_push_failed.txt" ] && { echo "failed files:"; cat "$SRC/../ftp_push_failed.txt"; rm -f "$SRC/../ftp_push_failed.txt"; exit 1; }
echo "done"
