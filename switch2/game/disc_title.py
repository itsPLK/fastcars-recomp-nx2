#!/usr/bin/env python3
"""The game's title from the player's own disc, for the start screen and the homebrew menu: the
"title name" field of Disc 1's nxeart package (an Xbox 360 STFS/PIRS header: UTF-16BE at 0x1691,
0x80 bytes). The repository never contains the title; it exists only in the player's own build.

Writes OUT_DIR/game_title.h (#define GAME_TITLE "...") and OUT_DIR/game_title.txt, each rewritten
only when it changed. Without a usable nxeart, the project's own name is used.

usage: disc_title.py NXEART|- OUT_DIR
"""
import os
import sys

FALLBACK = "fastcars"
TITLE_AT, TITLE_LEN = 0x1691, 0x80


def read_title(path):
    try:
        with open(path, "rb") as f:
            data = f.read(TITLE_AT + TITLE_LEN)
    except OSError:
        return None
    if data[:4] not in (b"PIRS", b"LIVE", b"CON ") or len(data) < TITLE_AT + TITLE_LEN:
        return None
    text = data[TITLE_AT:TITLE_AT + TITLE_LEN].decode("utf-16-be", "replace").split("\x00")[0]
    # The console's text screen draws ASCII only.
    text = "".join(c if 0x20 <= ord(c) < 0x7F else "?" for c in text).strip()
    return text or None


def write_if_changed(path, content):
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == content:
                return
    except OSError:
        pass
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


def main():
    src, out = sys.argv[1], sys.argv[2]
    title = (read_title(src) if src != "-" else None) or FALLBACK
    os.makedirs(out, exist_ok=True)
    escaped = title.replace("\\", "\\\\").replace('"', '\\"')
    write_if_changed(os.path.join(out, "game_title.h"),
                     f'#pragma once\n#define GAME_TITLE "{escaped}"\n')
    write_if_changed(os.path.join(out, "game_title.txt"), title + "\n")
    print(f"disc_title: {'from ' + src if title != FALLBACK else 'fallback name'}")


if __name__ == "__main__":
    main()
