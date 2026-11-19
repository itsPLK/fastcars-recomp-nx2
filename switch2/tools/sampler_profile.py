#!/usr/bin/env python3
"""Summarise logs/sampler.txt (Switch 2 flight recorder) into a per-thread profile.

usage: profile.py sampler.txt syms.txt [first_round] [last_round]
syms.txt: `llvm-nm -n --defined-only fastcars_switch2` (anchor rex_switch2_sampler_anchor).
"""
import bisect
import collections
import re
import sys

samp, symf = sys.argv[1], sys.argv[2]
r0 = int(sys.argv[3]) if len(sys.argv) > 3 else 0
r1 = int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 60

addrs, names = [], []
anchor_elf = None
for line in open(symf):
    p = line.split()
    if len(p) < 3 or p[1] not in "TtWw":
        continue
    a = int(p[0], 16)
    addrs.append(a)
    names.append(p[2])
    if p[2] == "rex_switch2_sampler_anchor":
        anchor_elf = a

def sym(elf_addr):
    i = bisect.bisect_right(addrs, elf_addr) - 1
    return names[i] if i >= 0 else "?"

thr = re.compile(r'tid=(\d+) name="([^"]*)" pc=0x([0-9a-f]+) lr=0x([0-9a-f]+).*?(?: bt:(.*))?$')
base = None
rnd = 0
self_c = collections.defaultdict(collections.Counter)
incl_c = collections.defaultdict(collections.Counter)
total = collections.Counter()
rounds = 0
for line in open(samp, errors="replace"):
    if line.startswith("rex_switch2_sampler_anchor="):
        rt = int(line.split("=")[1].split()[0], 16)
        base = rt - anchor_elf
        continue
    if line.startswith("=== round"):
        rnd = int(line.split()[2])
        if r0 <= rnd <= r1:
            rounds += 1
        continue
    if not (r0 <= rnd <= r1):
        continue
    m = thr.search(line)
    if not m:
        continue
    tid, name, pc = m.group(1), m.group(2), int(m.group(3), 16)
    key = f"{name}#{tid}"
    total[key] += 1
    f = sym(pc - base)
    self_c[key][f] += 1
    seen = {f}
    incl_c[key][f] += 1
    for a in (m.group(5) or "").split():
        g = sym(int(a, 16) - base)
        if g not in seen:
            seen.add(g)
            incl_c[key][g] += 1

print(f"rounds {rounds}")
for key, n in total.most_common():
    top = self_c[key].most_common(8)
    idle = sum(c for f, c in self_c[key].items() if "svc" in f.lower() or "Wait" in f or "wait" in f)
    print(f"\n== {key}: {n} samples, self-in-wait-ish {100*idle/n:.0f}%")
    for f, c in top:
        print(f"   self {100*c/n:5.1f}%  {f[:110]}")
    print("   -- inclusive:")
    for f, c in incl_c[key].most_common(10):
        print(f"   incl {100*c/n:5.1f}%  {f[:110]}")
