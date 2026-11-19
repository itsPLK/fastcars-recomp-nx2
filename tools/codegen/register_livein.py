#!/usr/bin/env python3
"""Find guest functions that break when registers become C++ locals (ReXGlue's *_as_local options).

With non_volatile_as_local / non_argument_as_local / cr_as_local / ctr_as_local / reserved_as_local,
each generated function keeps those registers in its own locals instead of PPCContext. That is only
correct for functions that follow the PowerPC calling convention. Two shapes do not:

  live-in   the function reads such a register before writing it on some path from its entry,
            i.e. it expects the caller's value (code split off another function and reached by a
            branch, or an exception funclet that runs on its owner's frame);
  live-out  after a call, the function reads a volatile register (r0, r11, r12, cr0/1/5-7, ctr,
            ...) before writing it, i.e. it expects a value the callee left there.

This runs a forward "written on every path" dataflow over the code that codegen generated WITHOUT
the options (every register still ctx.X), per function, over labels / gotos / conditional
returns / switch tables / conditional tail-call blocks. Reads that cannot change the result (xer
reads inside compare, subfe rX,rY,rY's ~a + a) are ignored. Output: one line per function,
"name livein=... liveout=...", and with --toml the share_registers list of the live-in ones
(fastcars/codegen_share_registers.toml).

usage: register_livein.py GENERATED_DIR [--toml OUT.toml]
"""
import argparse
import glob
import os
import re
import sys

TOK = re.compile(r"\bctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|reserved)\b")
FN = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
LABEL = re.compile(r"^(loc_[0-9A-F]+):$")
GOTO = re.compile(r"goto (loc_[0-9A-F]+);")
CALL = re.compile(r"^\s*(?:if \(.*\) \{\s*)?(\w+)\(ctx, base\);")
INDIRECT = "REX_CALL_INDIRECT_FUNC("

NONVOL = {f"r{i}" for i in range(14, 32)} | {f"f{i}" for i in range(14, 32)} | \
         {f"v{i}" for i in list(range(14, 32)) + list(range(64, 128))} | {"cr2", "cr3", "cr4"}
VOLATILE_LOCAL = {"r0", "r2", "r11", "r12", "f0"} | {f"v{i}" for i in range(32, 64)} | \
                 {"cr0", "cr1", "cr5", "cr6", "cr7", "ctr"}  # not reserved: never localized
LOCALIZABLE = NONVOL | VOLATILE_LOCAL


def writes_reads(line):
    """(written, read) register sets for one generated statement."""
    s = line.strip()
    w, r = set(), set()
    toks = list(TOK.finditer(s))
    if not toks:
        return w, r
    first = toks[0]
    lhs_write = False
    if first.start() == 0:
        rest = s[first.end():]
        m = re.match(r"(\.\w+)?(\[[^\]]*\])?\s*(=|\|=|&=|\^=|\+=|-=)(?!=)", rest)
        if m:
            lhs_write = True
            w.add(first.group(1))
            if m.group(3) != "=" or m.group(2):  # compound or element assignment: also a read
                r.add(first.group(1))
        elif re.match(r"\.(compare|setFromMask)\b", rest):
            lhs_write = True
            w.add(first.group(1))
    if s.startswith("--ctx.") or s.startswith("++ctx."):
        reg = toks[0].group(1)
        w.add(reg)
        r.add(reg)
        lhs_write = True
    m = re.match(r"simde_mm_store_(?:ps|si128|ss|pd)\(\s*(?:\(simde__m128i\*\))?ctx\.(v\d+)\.", s)
    if m:
        w.add(m.group(1))
        toks = [t for t in toks if not (t.group(1) == m.group(1) and t.start() == s.find("ctx." + m.group(1)))]
        lhs_write = True
    for i, t in enumerate(toks):
        if lhs_write and i == 0 and t.group(1) in w and not s.startswith(("--", "++")):
            continue
        reg = t.group(1)
        if reg == "xer":
            continue  # SO/CA reads: zero in a fresh local and no function passes them on
        r.add(reg)
    # Saving a register to the stack frame (prologue, __save* inlined, vector saves through ea) is
    # not a use of its value: the epilogue only loads it back.
    if re.match(r"(REX_STORE_U(32|64)\((ctx\.r1\.u32 \+ -?\d+|ea),|"
                r"simde_mm_store_si128\(\(simde__m128i\*\)REX_RAW_ADDR\(ea\),)", s):
        vals = [reg for reg in r if reg != "r1"]
        if len(vals) == 1 and vals[0] in NONVOL:
            r.discard(vals[0])
    # A vector instruction that merges into its destination (vrlimi128's blend, per-element
    # writes such as vlogefp's log2f per lane, vsel into itself) reads the destination's old
    # lanes; a register not written yet holds garbage the code never looks at.
    if w:
        for reg in list(r & w):
            if reg.startswith("v"):
                r.discard(reg)
    m = re.match(r"ctx\.(v\d+)\.\w+\[\d+\] =", s)
    if m:
        r.discard(m.group(1))
    # subfe rD,rA,rA: ~a + a + ca is independent of a
    for m in re.finditer(r"~ctx\.(r\d+)\.u32 \+ ctx\.\1\.u32", s):
        if s.count("ctx." + m.group(1) + ".") == 4:
            r.discard(m.group(1))
    return w, r


def analyze(name, lines):
    n = len(lines)
    labels = {}
    for i, l in enumerate(lines):
        m = LABEL.match(l.strip())
        if m:
            labels[m.group(1)] = i
    succ = [[] for _ in range(n)]
    depth_stack = []
    for i, l in enumerate(lines):
        s = l.strip()
        term = False
        for g in GOTO.finditer(s):
            if g.group(1) in labels:
                succ[i].append(labels[g.group(1)])
        if re.match(r"^goto loc_", s) or s == "return;" or s.startswith("__builtin_trap") or \
                re.match(r"^REX_FATAL\(", s):
            term = True
        if s.endswith("{") and (s.startswith("if (") or s.startswith("switch (")):
            depth_stack.append((i, s.startswith("switch (")))
        elif s.startswith("}") and depth_stack:
            j, is_switch = depth_stack.pop()
            if not is_switch and i + 1 < n:
                succ[j].append(i + 1)  # condition false: skip the block
        if s.startswith("case ") or s.startswith("default:"):
            for j, is_switch in reversed(depth_stack):
                if is_switch:
                    succ[j].append(i)
                    break
        if s.startswith("switch ("):
            term = True  # only via case edges
        if not term and i + 1 < n:
            succ[i].append(i + 1)
    info = []
    for i, l in enumerate(lines):
        w, r = writes_reads(l)
        s = l.strip()
        c = CALL.match(l)
        is_call = bool(c) or INDIRECT in s
        callee = c.group(1) if c else None
        if callee and callee.startswith(("__save", "__rest")):
            is_call = False  # elided under non_volatile_as_local; they clobber nothing else
        # r12 holding the return address (mflr r12 / lwz r12,N(r1) before mtlr) or a save-area
        # pointer for the __save/__rest helpers (addi r12,r1,-N) is no result
        w_ret = w - {"r12"} if re.match(r"ctx\.r12\.(u64 = (ctx\.lr|REX_LOAD_U32\(ctx\.r1\.u32 \+ -?\d+\))|s64 = ctx\.r1\.s64 \+ -?\d+);", s) else w
        is_ret = s == "return;" or bool(re.match(r"^if \(.*\) return;$", s))
        info.append((frozenset(w & LOCALIZABLE), frozenset(r & LOCALIZABLE), is_call,
                     frozenset(w_ret & LOCALIZABLE), is_ret))

    def must_written(entry, use_ret_writes=False):
        """Registers written on every path to each statement (calls clobber the volatile ones)."""
        inset = [None] * n
        inset[0] = entry
        work = [0]
        while work:
            i = work.pop()
            w, r, is_call, w_ret, _ = info[i]
            out = inset[i] | (w_ret if use_ret_writes else w)
            if is_call:
                out = (out - VOLATILE_LOCAL) | ((w_ret if use_ret_writes else w) & VOLATILE_LOCAL)
            for j in succ[i]:
                nxt = out if inset[j] is None else (inset[j] & out)
                if inset[j] is None or nxt != inset[j]:
                    inset[j] = nxt
                    work.append(j)
        return inset

    if n == 0:
        return set(), set(), set()
    fresh = must_written(frozenset())          # entry: nothing defined (live-in + live-out reads)
    defined = must_written(frozenset(LOCALIZABLE))  # entry: everything (only call clobbers left)
    livein, liveout = set(), set()
    for i in range(n):
        r = info[i][1]
        if fresh[i] is not None:
            livein |= r - fresh[i]
        if defined[i] is not None:
            liveout |= r - defined[i]
    livein -= liveout
    # Registers a function hands back: written after its last call on every path to a return.
    ret = must_written(frozenset(), use_ret_writes=True)
    returned = None
    for i in range(n):
        if info[i][4] and ret[i] is not None:
            rs = ret[i] | (info[i][3] if lines[i].strip() != "return;" else frozenset())
            returned = set(rs) if returned is None else returned & rs
    returned = (returned or set()) & VOLATILE_LOCAL
    return livein, liveout, returned


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("generated")
    ap.add_argument("--toml")
    a = ap.parse_args()
    results = []
    writes, calls, probes = {}, {}, set()
    for path in sorted(glob.glob(os.path.join(a.generated, "fastcars_recomp.*.cpp"))):
        cur, body = None, []
        for line in open(path, errors="replace"):
            m = FN.match(line)
            if m:
                cur, body = m.group(1), []
                continue
            if cur is None:
                continue
            if line.startswith("}"):
                wr, cl = set(), set()
                for l in body:
                    wr |= writes_reads(l)[0]
                    c = CALL.match(l)
                    if c:
                        cl.add(c.group(1))
                calls[cur] = cl
                if body and body[0].strip() == "ctx.r11.s64 = static_cast<int64_t>(-ctx.r12.u64);" and \
                        not any(l.strip().startswith(("REX_STORE", "simde_mm_store_si128((simde__m128i*)REX_RAW"))
                                for l in body):
                    probes.add(cur)
                if not (cur.startswith("__save") or cur.startswith("__rest")):
                    li, lo, rt = analyze(cur, body)
                    writes[cur] = rt
                    if li or lo:
                        results.append((cur, li, lo))
                cur = None
                continue
            if line.lstrip().startswith("//") or line.strip() == "REX_FUNC_PROLOGUE();" or \
                    line.strip().startswith("uint32_t ea{}"):
                continue
            body.append(line.rstrip("\n"))
    # A live-out read is a value MSVC's whole-program optimisation keeps across a call whose
    # callee it knows leaves the register alone: locals keep that correct (the callee cannot touch
    # the caller's local). Marking the caller would not help a callee that really returns a value
    # there, and marking callees by "writes it on every path" catches leaf CRT routines that use
    # f0/r11 as scratch (pow, fmod: hundreds of call sites). So only live-in functions share.
    # The stack probe (__chkstk: r12 = frame size, touches each page below r1) reads r12 from its
    # caller, but only to touch pages: with a local r12 of 0 it returns at once, and the caller's
    # own r12 is untouched, so it needs no hand-over (it has hundreds of call sites).
    # Vector live-ins are merges of a not-yet-written register's garbage lanes (vrlimi128 v56,v127
    # before v127 is loaded: the lanes are never used): vector arguments travel in v1-v13, which
    # stay in ctx anyway. Split-off code that really continues a caller also reads its GPRs.
    results = [(name, {x for x in li if not x.startswith("v")}, lo, "") for name, li, lo in results]
    results = [(name, li, lo, why) for name, li, lo, why in results
               if li and not (li == {"r12"} and name in probes)]
    for name, li, lo, why in results:
        print(f"{name} livein={','.join(sorted(li)) or '-'} liveout={','.join(sorted(lo)) or '-'} {why}")
    print(f"{len(results)} functions", file=sys.stderr)
    if a.toml:
        with open(a.toml, "w") as f:
            f.write("# Generated by tools/codegen/register_livein.py: functions that read a register\n"
                    "# their caller left in PPCContext (live-in: code split off another function, SEH\n"
                    "# funclets). They keep every register in ctx; their call sites hand theirs over.\n"
                    "[functions]\n")
            for name, li, lo, why in sorted(results):
                addr = name[4:] if name.startswith("sub_") else None
                if addr:
                    f.write(f"0x{addr} = {{ share_registers = true }}  # in: {','.join(sorted(li)) or '-'}"
                            f" out: {','.join(sorted(lo)) or '-'}{' ' + why if why else ''}\n")


if __name__ == "__main__":
    main()
