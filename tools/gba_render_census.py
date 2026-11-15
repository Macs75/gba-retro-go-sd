#!/usr/bin/env python3
"""gba_render_census.py -- summarise tools/gba_render_census.sh output.

  tools/gba_render_census.py <out dir> [--top N]

Per game: text-layer pixels per frame, split into the plain 16-bit
full-colour instances (FULL16), the 32-bit indexed/stacked instances that a
blended screen draws through (BLEND32), and the 16-bit indexed ones (INDX16);
then the games ranked by BLEND32 per frame and the set's totals.
"""
import os, re, sys

def load(d):
    out = {}
    for f in sorted(os.listdir(d)):
        g = {}
        frames = 0
        for l in set(open(os.path.join(d, f), errors='replace')):
            m = re.match(r'RCENSUS text<(u16|u32),(\w+),(\w+)> pix=(\d+) frames=(\d+)', l)
            if not m:
                continue
            k = 'FULL16' if (m[1], m[2]) == ('u16', 'FULLCOLOR') else \
                'BLEND32' if m[1] == 'u32' else 'INDX16'
            g[k] = g.get(k, 0) + int(m[4])
            frames = int(m[5])
        if frames:
            out[f[:-4]] = {k: v / frames for k, v in g.items()}
    return out

def main():
    d = sys.argv[1]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 30
    G = load(d)
    print('%d games; text-layer pixels per frame (one full 240x160 layer = 38400)' % len(G))
    used = [g for g in G if G[g].get('BLEND32', 0) > 0]
    print('games that draw any BLEND32 pixels: %d of %d' % (len(used), len(G)))
    for lim in (38400, 19200, 3840):
        print('  BLEND32 >= %5d per frame (%.1f layers): %d games' % (lim, lim / 38400, sum(1 for g in G if G[g].get('BLEND32', 0) >= lim)))
    tot = {k: sum(G[g].get(k, 0) for g in G) for k in ('FULL16', 'BLEND32', 'INDX16')}
    s = sum(tot.values())
    print('set total share: ' + '  '.join('%s %.1f%%' % (k, 100 * v / s) for k, v in tot.items()))
    print('\n%-44s %8s %8s %8s  %s' % ('game (by BLEND32 per frame)', 'FULL16', 'BLEND32', 'INDX16', 'BLEND32 share'))
    for g in sorted(G, key=lambda g: -G[g].get('BLEND32', 0))[:top]:
        v = G[g]; t = sum(v.values()) or 1
        print('%-44s %8.0f %8.0f %8.0f  %5.1f%%' % (g[:44], v.get('FULL16', 0), v.get('BLEND32', 0), v.get('INDX16', 0), 100 * v.get('BLEND32', 0) / t))

if __name__ == '__main__':
    main()
