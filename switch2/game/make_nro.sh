#!/bin/sh
# Packs the linked ELF into fastcars.nro (CMake POST_BUILD step).
#   make_nro.sh NACPTOOL ELF2NRO ELF OUT_DIR SOURCE_DIR
# Version: v<VERSION>-<commit> (make_version.sh); author "local-build". Icon:
# OUT_DIR/icon.jpg (256x256 JPEG, made from the player's own disc by make_icon.py) when present.
set -e
NACPTOOL=$1 ELF2NRO=$2 ELF=$3 OUT=$4 SRC=$5
VERSION=$(sh "$SRC/switch2/game/make_version.sh" "$SRC" -)
# Title: the game's title from the player's own disc (disc_title.py), else the project's name.
TITLE=$(head -n 1 "$OUT/game_title.txt" 2>/dev/null || true)
# Author: "local-build" (no project name or link next to the game's title).
"$NACPTOOL" --create "${TITLE:-fastcars}" "local-build" "$VERSION" "$OUT/fastcars.nacp"
set -- --nacp="$OUT/fastcars.nacp"
[ -s "$OUT/icon.jpg" ] && set -- "$@" --icon="$OUT/icon.jpg"
"$ELF2NRO" "$ELF" "$OUT/fastcars.nro" "$@"
echo "fastcars.nro: version $VERSION$([ -s "$OUT/icon.jpg" ] && echo ', icon from icon.jpg')"
