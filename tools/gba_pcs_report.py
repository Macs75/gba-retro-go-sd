#!/usr/bin/env python3
"""gba_pcs_report.py -- where the device's CPU spent its time during a
replay, from the PC sampler of a PROF build (src/porting/gba_prof.c).

  tools/gba_pcs_report.py <rom.pcs> <gba_core.elf> [--block N] [--top N] [--fn NAME]

<rom.pcs>: written next to the ROM by a PROF build while a replay runs (one
block per 1800 replay frames, then the rest); pull it with gnwmanager
sdpull. <gba_core.elf>: the ELF of THAT build. Prints, per block (or only
--block N), the share of the samples per function, then the --top hottest
buckets (16 bytes in ITCM, 32 elsewhere) with their disassembly (or only
those of functions whose name contains --fn). "firmware" is everything
outside the core: interrupts, HAL, ABI calls and the audio wait's sleep.
Functions come from the symbol table (debug info is unreliable in ITCM).
"""
import bisect, collections, re, struct, subprocess, sys

HDR = "<11I"
XIP_LINK = 0xDEC00000

def blocks(path):
    d = open(path, "rb").read()
    o = 0
    while o + struct.calcsize(HDR) <= len(d):
        (magic, ver, f0, f1, xip, ram_lo, itcm_n, ram_n, xip_n, other, total) = struct.unpack_from(HDR, d, o)
        if magic != 0x53435047:
            sys.exit(f"bad block at {o}")
        o += struct.calcsize(HDR)
        arr = struct.unpack_from(f"<{itcm_n + ram_n + xip_n}H", d, o)
        o += 2 * (itcm_n + ram_n + xip_n)
        b = collections.Counter()
        for i, n in enumerate(arr[:itcm_n]):
            if n: b[(i * 16, 16)] = n
        for i, n in enumerate(arr[itcm_n:itcm_n + ram_n]):
            if n: b[(ram_lo + i * 32, 32)] = n
        for i, n in enumerate(arr[itcm_n + ram_n:]):
            if n: b[(XIP_LINK + i * 32, 32)] = n
        yield {"from": f0, "to": f1, "other": other, "total": total, "b": b,
               "sat": sum(1 for n in arr if n == 0xFFFF)}

def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    path, elf = a[0], a[1]
    want, top, fn = None, 25, None
    i = 2
    while i < len(a):
        if a[i] == "--block": want = int(a[i + 1])
        elif a[i] == "--top": top = int(a[i + 1])
        elif a[i] == "--fn": fn = a[i + 1]
        i += 2
    syms = []
    for l in subprocess.run(["arm-none-eabi-nm", "-n", "-S", "-C", elf], capture_output=True,
                            text=True).stdout.splitlines():
        p = l.split(None, 3)
        if len(p) == 4 and p[2].lower() in ("t", "w"):
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    starts = [s[0] for s in syms]
    def fn_of(x):
        k = bisect.bisect_right(starts, x) - 1
        if k >= 0 and syms[k][0] <= x < syms[k][0] + max(syms[k][1], 1):
            return syms[k][2]
        return "?%08x" % x
    def dis(addr, size):
        out = subprocess.run(["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", elf,
                              f"--start-address={addr:#x}", f"--stop-address={addr + size:#x}"],
                             capture_output=True, text=True).stdout
        return [m.group(2).strip() for m in re.finditer(r"^\s*([0-9a-f]+):\s+(.*)$", out, re.M)]
    for k, b in enumerate(blocks(path)):
        if want is not None and k != want:
            continue
        t = b["total"] or 1
        print(f"== block {k}: replay frames {b['from']}-{b['to']}, {b['total']} samples"
              + (f", {b['sat']} buckets SATURATED" if b["sat"] else ""))
        byfn = collections.Counter()
        for (x, sz), n in b["b"].items():
            byfn[fn_of(x)] += n
        byfn["(firmware: interrupts, HAL, ABI, the audio wait's sleep)"] = b["other"]
        for f, n in byfn.most_common(22):
            print(f"  {100.0 * n / t:5.1f}%  {f[:100]}")
        print(f"  hottest {'buckets' if not fn else 'buckets of ' + fn}:")
        hot = [(n, x, sz) for (x, sz), n in b["b"].items() if not fn or fn in fn_of(x)]
        for n, x, sz in sorted(hot, reverse=True)[:top]:
            ins = dis(x, sz)
            print(f"  {100.0 * n / t:5.2f}%  {x:08x} {fn_of(x)[:28]:28s} {' ; '.join(ins)[:150]}")
        print()

main()
