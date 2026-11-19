#!/usr/bin/env python3
# Real fps per 300-frame window from fastcars.log "presented N frames" lines (gameplay = record avg > 8 ms).
# usage: log_fps.py fastcars.log [...]
import re, sys
for f in sys.argv[1:]:
    prev = None; out = []
    for line in open(f):
        m = re.search(r'^\[\S+ (\d+):(\d+):([\d.]+)\].*presented (\d+) frames.*record avg ([\d.]+)', line)
        if not m: continue
        t = int(m[1])*3600 + int(m[2])*60 + float(m[3]); n = int(m[4]); rec = float(m[5])
        if prev and n > prev[1]:
            fps = (n - prev[1]) / (t - prev[0])
            if rec > 8: out.append((fps, rec))   # gameplay windows (record > 8 ms)
        prev = (t, n)
    if out:
        fs = sorted(x[0] for x in out)
        print(f"{f}: {len(out)} gameplay windows of 300 frames: fps median {fs[len(fs)//2]:.1f}, min {fs[0]:.1f}, max {fs[-1]:.1f}; record avg {sum(x[1] for x in out)/len(out):.1f} ms")
