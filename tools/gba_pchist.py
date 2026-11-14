#!/usr/bin/env python3
"""gba_pchist.py -- read a host PC histogram (GBA_PCHIST=1) and disassemble
its hot spots from the ROM.

  tools/gba_pchist.py <rom.gba> <log> [--window N] [--context 8]

<log>: the host's output with PCHIST lines (src/porting/gba_pchist.c).
--window N: the Nth PCHIST block (0-based; default: the busiest).
--classify NAME: one census line instead (tools/gba_idle_census.sh):
  NAME | poll% | instructions per frame | region split | hottest loop
  poll% = the window's instructions in POLL loops: a backward branch over
  at most 16 instructions with no store, call, SWI or PC load in between
  (it only reads and compares: every pass is the same until what it reads
  changes) -- the loops a generic skip can jump over.
Prints the block's region split, then the top PCs grouped into runs of
neighbouring addresses (a loop shows up as several PCs with equal counts),
each run disassembled from the ROM (Thumb or ARM as recorded) with every
instruction's count. IWRAM/EWRAM code is not in the ROM: only its opcode
from the log is decoded.
"""
import re, sys
import capstone

def blocks(path):
    cur = None
    for line in open(path, errors="replace"):
        m = re.match(r"PCHIST frames=(\d+)-(\d+) instr=(\d+) per frame, region split: (.*)", line)
        if m:
            cur = {"from": int(m[1]), "to": int(m[2]), "instr": int(m[3]), "split": m[4].strip(), "pcs": []}
            yield cur
            continue
        m = re.match(r"PCHIST pc=([0-9a-f]+) ([AT]) n=(\d+) pct=([\d.]+) op=([0-9a-f]+)", line)
        if m and cur is not None:
            cur["pcs"].append((int(m[1], 16), m[2] == "T", int(m[3]), float(m[4]), int(m[5], 16)))

STORE = ("str", "stm", "push")
CALL = ("bl", "blx", "bx", "svc", "swi", "pop", "ldm")

def loop_of(rom, run, counts, md):
    """the backward branch closing the loop the run sits in, and its body"""
    thumb = counts[run[0]][2]
    step = 2 if thumb else 4
    lo, hi = run[0] - 16 * step, run[-1] + 4 * step
    if 0x08000000 <= run[0] < 0x0e000000:
        o = lo & 0x1ffffff
        ins = list(md.disasm(rom[o:o + (hi - lo)], lo))
    else:
        ins = []
        for p in run:
            raw = counts[p][3].to_bytes(step, "little")
            ins += list(md.disasm(raw, p))
    best = None
    for i in ins:
        if not i.mnemonic.startswith("b") or i.mnemonic.startswith(("bl", "bx", "bic")):
            continue
        try:
            t = int(i.op_str.lstrip("#"), 16)
        except ValueError:
            continue
        if t <= i.address and t >= run[0] - 16 * step and i.address >= run[-1] - 4 * step:
            if best is None or t > best[0]:
                best = (t, i.address)
    if not best:
        return None
    body = [i for i in ins if best[0] <= i.address <= best[1]]
    if len(body) > 16 or not body or body[0].address != best[0]:
        return ("long", best, body)
    for i in body:
        if i.mnemonic.startswith(STORE):
            return ("store", best, body)
        if i.mnemonic.startswith(CALL) and not i.mnemonic.startswith("bls"):
            return ("call", best, body)
        if "pc" in i.op_str.split(",")[0]:
            return ("call", best, body)
    return ("poll", best, body)

def classify(rom, b, name):
    counts = {pc: (n, pct, th, op) for pc, th, n, pct, op in b["pcs"]}
    pcs = sorted(counts)
    runs, cur = [], []
    for pc in pcs:
        if cur and pc - cur[-1] > 16:
            runs.append(cur); cur = []
        cur.append(pc)
    if cur: runs.append(cur)
    th_md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    arm_md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    poll, top = 0.0, None
    for r in sorted(runs, key=lambda r: -sum(counts[p][1] for p in r)):
        share = sum(counts[p][1] for p in r)
        l = loop_of(rom, r, counts, th_md if counts[r[0]][2] else arm_md)
        kind = l[0] if l else "noloop"
        if kind == "poll":
            poll += share
        if top is None:
            body = "; ".join(f"{i.mnemonic} {i.op_str}" for i in l[2][:6]) if l else ""
            top = f"{r[0]:08x} {share:.1f}% {kind}: {body}"
    split = re.sub(r"(\w+)=([\d.]+)%", lambda m: f"{m[1][0]}{float(m[2]):.0f}", b["split"])
    print(f"{name[:48]:48s} | {poll:5.1f} | {b['instr']:6d} | {split} | {top}")

def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    rom = open(a[0], "rb").read()
    bl = list(blocks(a[1]))
    if len(a) >= 4 and a[2] == "--classify":
        if not bl:
            print(f"{a[3][:48]:48s} |  n/a  |    n/a | no PCHIST block (crashed or too slow)")
            return
        classify(rom, max(bl, key=lambda x: x["instr"]), a[3])
        return
    win = None
    ctx = 8
    i = 2
    while i < len(a):
        if a[i] == "--window": win = int(a[i + 1]); i += 2
        elif a[i] == "--context": ctx = int(a[i + 1]); i += 2
        else: sys.exit(__doc__)
    b = bl[win] if win is not None else max(bl, key=lambda x: x["instr"])
    print(f"frames {b['from']}-{b['to']}: {b['instr']} instructions per frame; {b['split']}")
    counts = {pc: (n, pct, th, op) for pc, th, n, pct, op in b["pcs"]}
    # runs: PCs within 16 bytes of each other
    pcs = sorted(counts)
    runs, cur = [], []
    for pc in pcs:
        if cur and pc - cur[-1] > 16:
            runs.append(cur); cur = []
        cur.append(pc)
    if cur: runs.append(cur)
    runs.sort(key=lambda r: -sum(counts[p][1] for p in r))
    th_md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    arm_md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    for r in runs:
        share = sum(counts[p][1] for p in r)
        thumb = counts[r[0]][2]
        print(f"\n== {r[0]:08x}-{r[-1]:08x} {'Thumb' if thumb else 'ARM'}: {share:.2f}% of the window's instructions (top-list PCs only)")
        md = th_md if thumb else arm_md
        if 0x08000000 <= r[0] < 0x0e000000:
            lo = (r[0] & 0x1ffffff) - ctx * (2 if thumb else 4)
            hi = (r[-1] & 0x1ffffff) + ctx * (2 if thumb else 4)
            lo = max(lo, 0)
            for ins in md.disasm(rom[lo:hi], 0x08000000 + lo):
                c = counts.get(ins.address)
                mark = f"{c[0]:>9} {c[1]:5.2f}%" if c else " " * 16
                print(f"  {mark}  {ins.address:08x}: {ins.mnemonic:8s} {ins.op_str}")
        else:
            for p in r:
                n, pct, th, op = counts[p]
                raw = op.to_bytes(2 if th else 4, "little")
                d = next(md.disasm(raw, p), None)
                txt = f"{d.mnemonic} {d.op_str}" if d else f"op={op:x}"
                print(f"  {n:>9} {pct:5.2f}%  {p:08x}: {txt}   (RAM code: opcode from the log)")

main()
