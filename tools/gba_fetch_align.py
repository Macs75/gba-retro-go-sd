#!/usr/bin/env python3
"""gba_fetch_align.py -- score how the interpreter's hot paths sit on the
Cortex-M7's 64-bit ITCM instruction fetch.

  tools/gba_fetch_align.py <gba_core.elf> <mix dir> [<mix dir> ...] [--detail]

The M7 fetches ITCM 8 bytes at a time: a 32-bit Thumb-2 instruction at an
address = 6 mod 8 straddles two fetches. Where GCC happens to lay out
execute_arm moved the interpreter's device cost by ~5% on its own (Kirby,
2026-11-15: same work, 0.7 fewer straddles per Thumb instruction), so a
placement is chosen by this score, not by luck.

For every opcode group the script walks the straight-line path from its
dispatch target to the next dispatch:
  Thumb: thumb_tbl's target -> the tail's computed jump (bx rN);
  ARM:   the ARM switch's tbh target -> the post path -> the loop top ->
         the next tbh.
Unconditional branches are followed; conditional branches are not taken
(the slow paths: a page change, a jump, a misaligned PC, an alert, the end
of the slice), except, on the ARM path, the post path's back edge to the
loop top (a conditional branch to the code just before the ARM dispatch).
It counts the instructions, the 32-bit ones, and the 32-bit ones at 6 mod 8
(straddles).

<mix dir>: host opcode histograms, one file per game, lines
  THOP <group hex> <count> <share>   (HOST_EXTRA_DEFS=-DGBA_THUMB_OPHIST)
  ARMOP <group hex> <count> <share>  (HOST_EXTRA_DEFS=-DGBA_ARM_OPHIST)
A game's THOP and ARMOP lines may sit in files of the same name in
different dirs. Each game counts equally; inside a game the ARM and Thumb
paths are weighted by its own instruction mix.

Prints straddles per executed instruction per game and their average
(lower is better), plus --detail per-group paths for the hottest groups.
"""
import collections, os, re, struct, subprocess, sys

def sh(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout

def load_elf(elf):
    nm = sh('arm-none-eabi-nm', '-S', elf)
    sym = {}
    for l in nm.splitlines():
        p = l.split()
        if len(p) == 4:
            sym[p[3]] = (int(p[0], 16), int(p[1], 16))
    ea, ea_size = sym['execute_arm']
    tbl = [v for k, v in sym.items() if k.endswith('thumb_tbl')]
    thumb_tbl = tbl[0][0] if tbl else None
    tmp = elf + '.itcm.bin'
    subprocess.run(['arm-none-eabi-objcopy', '-O', 'binary', '-j', '.core_itcm', elf, tmp], check=True)
    itcm = open(tmp, 'rb').read()
    os.unlink(tmp)
    dis = sh('arm-none-eabi-objdump', '-d', '--start-address=%d' % ea,
             '--stop-address=%d' % (ea + ea_size), elf)
    ins = []
    for l in dis.splitlines():
        m = re.match(r'^\s+([0-9a-f]+):\s+((?:[0-9a-f]{4} ?){1,2})\s+(\S+)\s*(.*)$', l)
        if m and not m.group(3).startswith('.'):
            ins.append((int(m.group(1), 16), 2 * len(m.group(2).split()), m.group(3), m.group(4)))
    return ins, itcm, thumb_tbl

COND = re.compile(r'^b(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(\.w|\.n)?$')

def walker(ins):
    idx = {a: i for i, (a, _, _, _) in enumerate(ins)}
    def walk(start, stop, take=lambda a, t: False):
        i = idx.get(start)
        seen = set(); n = wide = strad = 0; path = []
        while i is not None and n < 400:
            a, sz, mn, args = ins[i]
            if a in seen:
                break
            seen.add(a); n += 1; path.append(a)
            if sz == 4:
                wide += 1
                if a % 8 == 6:
                    strad += 1
            if stop(a, mn, args) and n > 1:
                return n, wide, strad, True
            tgt = None
            if mn in ('b', 'b.w', 'b.n') or COND.match(mn):
                try:
                    tgt = int(args.split()[0], 16)
                except ValueError:
                    tgt = None
            if mn in ('b', 'b.w', 'b.n'):
                i = idx.get(tgt); continue
            if COND.match(mn) and tgt is not None and take(a, tgt):
                i = idx.get(tgt); continue
            if mn in ('cbz', 'cbnz'):
                pass
            i += 1
        return n, wide, strad, False
    return walk

def mixes(dirs):
    games = collections.defaultdict(lambda: {'THOP': {}, 'ARMOP': {}, 'n': {'THOP': 0, 'ARMOP': 0}})
    for d in dirs:
        for f in sorted(os.listdir(d)):
            if not f.endswith('.txt'):
                continue
            g = games[f[:-4]]
            for l in open(os.path.join(d, f), errors='replace'):
                p = l.split()
                if len(p) == 4 and p[0] in ('THOP', 'ARMOP'):
                    g[p[0]][int(p[1], 16)] = int(p[2])
                    g['n'][p[0]] += int(p[2])
    return {k: v for k, v in games.items() if v['n']['THOP'] or v['n']['ARMOP']}

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    detail = '--detail' in sys.argv
    if len(args) < 2:
        print(__doc__); sys.exit(1)
    elf, dirs = args[0], args[1:]
    ins, itcm, thumb_tbl = load_elf(elf)
    walk = walker(ins)
    # ARM dispatch: the tbh right after "ubfx rX, rY, #20, #8"
    arm_tbh = None
    for i, (a, sz, mn, args_) in enumerate(ins):
        if mn == 'tbh' and any('#20, #8' in ins[j][3] for j in range(max(0, i - 6), i)):
            arm_tbh = a; break
    res = {}
    thumb_t = arm_t = None
    if thumb_tbl is not None:
        thumb_t = [t & ~1 for t in struct.unpack_from('<256I', itcm, thumb_tbl)]
        stop_t = lambda a, mn, args_: mn == 'bx' and args_.startswith('r')
        res['THOP'] = {op: walk(thumb_t[op], stop_t) for op in range(256)}
    if arm_tbh is not None:
        hw = struct.unpack_from('<256H', itcm, arm_tbh + 4)
        arm_t = [arm_tbh + 4 + 2 * h for h in hw]
        stop_a = lambda a, mn, args_: a == arm_tbh
        # the loop top (arm_loop_pc .. the fetch) sits just before the tbh
        take_a = lambda a, t: arm_tbh - 0x180 <= t < arm_tbh
        res['ARMOP'] = {op: walk(arm_t[op], stop_a, take_a) for op in range(256)}
    G = mixes(dirs)
    print('%s: execute_arm at 0x%x (%d mod 8)' % (os.path.basename(elf), ins[0][0], ins[0][0] % 8))
    tot = []
    for g, m in sorted(G.items()):
        num = den = 0.0
        insn = 0.0
        for kind in ('THOP', 'ARMOP'):
            if kind not in res:
                continue
            for op, c in m[kind].items():
                n, wide, strad, ok = res[kind][op]
                num += c * strad; insn += c * n; den += c
        if den:
            tot.append(num / den)
            print('  %-10s straddles %.3f / instruction   path %.1f instructions   (Thumb %4.1f%%)'
                  % (g, num / den, insn / den, 100.0 * m['n']['THOP'] / den))
    if tot:
        print('  AVERAGE    straddles %.3f / instruction  (%d games, equal weight)' % (sum(tot) / len(tot), len(tot)))
    if detail:
        for kind in ('THOP', 'ARMOP'):
            if kind not in res:
                continue
            agg = collections.Counter()
            for m in G.values():
                n_ = m['n'][kind] or 1
                for op, c in m[kind].items():
                    agg[op] += c / n_
            print('hottest %s groups:' % kind)
            for op, w in agg.most_common(12):
                n, wide, strad, ok = res[kind][op]
                print('   %02x  weight %.3f  path %3d  32-bit %3d  straddles %3d%s'
                      % (op, w / len(G), n, wide, strad, '' if ok else '  (walk did not reach the dispatch)'))

if __name__ == '__main__':
    main()
