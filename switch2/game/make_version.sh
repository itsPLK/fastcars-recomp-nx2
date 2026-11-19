#!/bin/sh
# The build's version: v<VERSION file>-<commit> ("-dirty" with uncommitted changes), e.g.
# v1.0.0-1a2b3c4d. Shown as the homebrew menu's version (make_nro.sh).
#   make_version.sh SOURCE_DIR OUT   writes OUT (fastcars_version.h), rewritten only when it changed
#   make_version.sh SOURCE_DIR -     prints the version
SRC=$1 OUT=$2
BASE=$(head -n 1 "$SRC/VERSION" 2>/dev/null | tr -d ' \r')
if H=$(git -c safe.directory='*' -C "$SRC" rev-parse --short=8 HEAD 2>/dev/null); then
  git -c safe.directory='*' -C "$SRC" diff --quiet HEAD -- 2>/dev/null || H="$H-dirty"
else
  # No git: the installer's copy (or a source zip) records the commit in COMMIT.
  H=$(head -n 1 "$SRC/COMMIT" 2>/dev/null | tr -d ' \r')
  case $H in ''|'$'*) H=unknown;; esac
fi
V="v${BASE:-0.0.0}-$H"
if [ "$OUT" = "-" ]; then echo "$V"; exit 0; fi
printf '#pragma once\n#define FASTCARS_VERSION "%s"\n' "$V" > "$OUT.tmp"
if cmp -s "$OUT.tmp" "$OUT"; then rm -f "$OUT.tmp"; else mv "$OUT.tmp" "$OUT"; fi
