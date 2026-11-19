#!/usr/bin/env python3
# Late audio frames (silence filled) per 10 s during gameplay (after the first frame with > 300 draws).
# usage: log_audio.py fastcars.log [...]
import re, sys
for f in sys.argv[1:]:
    gp_start = None; vals = []
    for line in open(f):
        m = re.search(r'^\[\S+ ([\d:.]+)\].*\[native\] frame \d+: (\d+) draws', line)
        if m and int(m[2]) > 300 and gp_start is None: gp_start = m[1]
        m = re.search(r'^\[\S+ ([\d:.]+)\].*audio: (\d+) of 1875', line)
        if m and gp_start and m[1] > gp_start: vals.append(int(m[2]))
    if vals: print(f"{f}: {len(vals)} gameplay audio windows, silent frames per 10 s: median {sorted(vals)[len(vals)//2]}, max {max(vals)}, zero in {sum(v==0 for v in vals)}")
