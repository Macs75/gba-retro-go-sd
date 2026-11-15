#!/usr/bin/env python3
"""gba_thumb_cost.py -- device cost per executed Thumb instruction, per
opcode group, for one PC-sampler block.

  tools/gba_thumb_cost.py <rom.pcs> <gba_core.elf> <block> <ops_from> <ops_to> [--top N]

<ops_from>/<ops_to>: THOP lines (host HOST_EXTRA_DEFS=-DGBA_THUMB_OPHIST,
stderr) of the same replay run to the block's first and last frame
(GBA_EXIT_FRAME); their difference is the block's executed Thumb mix.
Every ITCM sample on a Thumb handler path (tools/gba_thumb_path_diff.py:
the thumb_tbl target to its computed jump) is shared among the groups whose
paths hold it, by their instruction counts. Prints per group: share of the
block's samples, instructions, device cycles per instruction (1 kHz samples
of the core clock). Groups whose handler shares a lot of code show a
blended cost.
"""
import collections, importlib.util, os, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('td', os.path.join(here, 'gba_thumb_path_diff.py'))
td = importlib.util.module_from_spec(spec); spec.loader.exec_module(td)
CLOCK = 354e6

def block(path, k):
    d = open(path, 'rb').read(); o = 0; i = 0
    while o + 44 <= len(d):
        (magic, ver, f0, f1, xip, ram_lo, itcm_n, ram_n, xip_n, other, total) = struct.unpack_from('<11I', d, o)
        o += 44
        arr = struct.unpack_from(f'<{itcm_n + ram_n + xip_n}H', d, o)
        o += 2 * (itcm_n + ram_n + xip_n)
        if i == k:
            return f0, f1, total, {j * 16: n for j, n in enumerate(arr[:itcm_n]) if n}
        i += 1
    sys.exit('no block %d' % k)

def ops(path):
    w = {}
    for l in open(path):
        p = l.split()
        if len(p) == 4 and p[0] == 'THOP':
            w[int(p[1], 16)] = int(p[2])
    return w

def main():
    pcs, elf, k, fa, fb = sys.argv[1:6]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
    f0, f1, total, itcm = block(pcs, int(k))
    A, B = ops(fa), ops(fb)
    mix = {g: B.get(g, 0) - A.get(g, 0) for g in B}
    P = td.paths(elf)
    owners = collections.defaultdict(set)
    for g, seq in P.items():
        for a, sz, mn, args in seq:
            owners[a & ~15].add(g)
    smp = collections.Counter()
    for a, n in itcm.items():
        gs = [g for g in owners.get(a, ()) if mix.get(g, 0) > 0]
        w = sum(mix[g] for g in gs)
        for g in gs:
            smp[g] += n * mix[g] / w
    tot_ins = sum(mix.values())
    tot_smp = sum(smp.values())
    print('block %s (frames %d-%d): %d samples; Thumb paths %.1f%% of them; %d Thumb instructions '
          '(%.0f per frame); %.1f device cycles per Thumb instruction on its path'
          % (k, f0, f1, total, 100 * tot_smp / total, tot_ins, tot_ins / (f1 - f0),
             tot_smp * CLOCK / 1000 / max(1, tot_ins)))
    print('  grp  samples%%  instr%%  cyc/instr')
    for g in sorted(smp, key=lambda g: -smp[g])[:top]:
        print('  %02x   %6.2f%%  %5.1f%%  %6.1f' % (g, 100 * smp[g] / total, 100 * mix[g] / tot_ins,
                                                  smp[g] * CLOCK / 1000 / mix[g]))

if __name__ == '__main__':
    main()
