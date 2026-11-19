#!/usr/bin/env python3
import bisect, collections, re, sys
"""Call chains (6 frames) of the samples of one thread whose PC is in one function.
usage: sampler_callers.py sampler.txt syms.txt "<thread name prefix>" <function> <first_round> <last_round>
syms.txt: llvm-nm -n --defined-only switch2/game/build/fastcars_switch2 (keep the ELF of the build that ran)."""
samp, symf, want_thread, want_fn = sys.argv[1:5]
r0, r1 = int(sys.argv[5]), int(sys.argv[6])
addrs, names, anchor = [], [], None
for line in open(symf):
    p = line.split()
    if len(p) < 3 or p[1] not in "TtWw": continue
    addrs.append(int(p[0], 16)); names.append(p[2])
    if p[2] == "rex_switch2_sampler_anchor": anchor = int(p[0], 16)
def sym(a):
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] if i >= 0 else "?"
thr = re.compile(r'name="([^"]*)" pc=0x([0-9a-f]+) lr=0x([0-9a-f]+).*?(?: bt:(.*))?$')
base = None; rnd = 0; chains = collections.Counter(); n = 0
for line in open(samp, errors="replace"):
    if line.startswith("rex_switch2_sampler_anchor="):
        base = int(line.split("=")[1].split()[0], 16) - anchor; continue
    if line.startswith("=== round"): rnd = int(line.split()[2]); continue
    if not (r0 <= rnd <= r1): continue
    m = thr.search(line)
    if not m or not m.group(1).startswith(want_thread): continue
    if sym(int(m.group(2), 16) - base) != want_fn: continue
    n += 1
    bt = [sym(int(a, 16) - base) for a in (m.group(4) or "").split()]
    chains[" < ".join(f[:60] for f in bt[:6])] += 1
print(want_thread, want_fn, n)
for c, k in chains.most_common(12): print(f"{k:5d}  {c}")
