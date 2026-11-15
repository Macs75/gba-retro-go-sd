#!/usr/bin/env python3
"""gba_ophist_sum.py -- summarise tools/gba_ophist_census.sh output: per ARM
(bits 27-20) and Thumb (opcode >> 8) group, its largest share of any one
game's executed instructions of that kind, the games it exceeds 0.1% in,
and the groups never above --cold (default 0.05%) in any game.

  tools/gba_ophist_sum.py <out dir> [--cold PCT] [--kind ARMOP|THOP]
"""
import collections, os, sys

d = sys.argv[1]
cold = float(sys.argv[sys.argv.index('--cold') + 1]) if '--cold' in sys.argv else 0.05
kinds = [sys.argv[sys.argv.index('--kind') + 1]] if '--kind' in sys.argv else ['ARMOP', 'THOP']
for kind in kinds:
    mx = collections.Counter(); ngames = collections.Counter(); who = {}
    games = 0
    for f in os.listdir(d):
        cnt = {}
        for l in open(os.path.join(d, f), errors='replace'):
            p = l.split()
            if len(p) == 4 and p[0] == kind:
                cnt[int(p[1], 16)] = int(p[2])
        t = sum(cnt.values())
        if not t:
            continue
        games += 1
        for g, c in cnt.items():
            s = 100.0 * c / t
            if s > mx[g]:
                mx[g] = s; who[g] = f[:-4]
            if s > 0.1:
                ngames[g] += 1
    coldg = [g for g in range(256) if mx[g] < cold]
    print('%s: %d games; %d of 256 groups never above %.2f%% in any game:' % (kind, games, len(coldg), cold))
    print('  ' + ' '.join('%02x' % g for g in coldg))
    warm = sorted((g for g in range(256) if cold <= mx[g] < 1.0), key=lambda g: mx[g])
    print('  low (max < 1%%): ' + ' '.join('%02x:%.2f%%' % (g, mx[g]) for g in warm))
