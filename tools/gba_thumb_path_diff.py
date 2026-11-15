#!/usr/bin/env python3
"""gba_thumb_path_diff.py -- compare the Thumb interpreter's hot paths of two
device builds, instruction by instruction.

  tools/gba_thumb_path_diff.py <a.elf> <b.elf> <mix file> [--top N] [--diff N]

For every Thumb opcode group, the straight-line path from its thumb_tbl
target to the tail's computed jump (bx rN): unconditional branches followed,
conditional ones not taken (the slow paths), calls (bl) not entered.

Prints, weighted by the mix file's executed Thumb instructions (THOP lines,
HOST_EXTRA_DEFS=-DGBA_THUMB_OPHIST), per executed instruction and per build:
instructions, 32-bit ones, loads and stores by kind (stack [sp], literal
pool [pc], other memory), calls; then the --top hottest groups side by side,
and --diff unified diffs of their normalised instruction sequences
(registers kept, addresses and branch targets dropped).

Why: the same Thumb source compiled in two contexts cost the device ~5%
differently (Kirby, builds 12 vs 13, 2026-11-15) with the same path length;
this shows what the compiler did differently.
"""
import collections, difflib, importlib.util, os, re, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('fa', os.path.join(here, 'gba_fetch_align.py'))
fa = importlib.util.module_from_spec(spec); spec.loader.exec_module(fa)

def paths(elf):
    ins, itcm, thumb_tbl = fa.load_elf(elf)
    idx = {a: i for i, (a, _, _, _) in enumerate(ins)}
    tgt = [t & ~1 for t in struct.unpack_from('<256I', itcm, thumb_tbl)]
    out = {}
    for op in range(256):
        i = idx.get(tgt[op]); seen = set(); seq = []
        while i is not None and len(seq) < 300:
            a, sz, mn, args = ins[i]
            if a in seen:
                break
            seen.add(a); seq.append((a, sz, mn, args))
            if mn == 'bx' and args.startswith('r'):
                break
            if mn in ('b', 'b.w', 'b.n'):
                try:
                    i = idx.get(int(args.split()[0], 16)); continue
                except ValueError:
                    break
            i += 1
        out[op] = seq
    return out

def norm(mn, args):
    args = re.sub(r'\s*;.*$', '', args)
    args = re.sub(r'\[pc, #-?\d+\]', '[pc]', args)
    if mn.startswith('b') and re.match(r'^[0-9a-f]+ <', args):
        args = '<tgt>'
    args = re.sub(r'^[0-9a-f]+ <[^>]*>$', '<tgt>', args)
    return (mn + ' ' + args).strip()

def stats(seq):
    s = collections.Counter()
    for a, sz, mn, args in seq:
        s['insn'] += 1
        if sz == 4: s['32-bit'] += 1
        if mn.startswith('bl') and not mn.startswith('bls') and mn not in ('blt', 'ble', 'blt.w', 'ble.w'):
            s['calls'] += 1
        m = re.match(r'^(ldr|str|ldm|stm|push|pop|pld)', mn)
        if m:
            kind = 'ld' if mn.startswith(('ldr', 'ldm', 'pop')) else 'st'
            if '[sp' in args or mn.startswith(('push', 'pop')):
                s[kind + ' stack'] += 1
            elif '[pc' in args:
                s[kind + ' literal'] += 1
            else:
                s[kind + ' other'] += 1
    return s

def main():
    a_elf, b_elf, mixf = sys.argv[1:4]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 12
    ndiff = int(sys.argv[sys.argv.index('--diff') + 1]) if '--diff' in sys.argv else 3
    w = {}
    for l in open(mixf, errors='replace'):
        p = l.split()
        if len(p) == 4 and p[0] == 'THOP':
            w[int(p[1], 16)] = int(p[2])
    tot = sum(w.values())
    A, B = paths(a_elf), paths(b_elf)
    keys = ['insn', '32-bit', 'ld stack', 'st stack', 'ld literal', 'ld other', 'st other', 'calls']
    agg = {'A': collections.Counter(), 'B': collections.Counter()}
    for op, c in w.items():
        for k, P in (('A', A), ('B', B)):
            for kk, v in stats(P[op]).items():
                agg[k][kk] += v * c / tot
    print('weighted per executed Thumb instruction (%s)' % os.path.basename(mixf))
    print('  %-12s %8s %8s %8s' % ('', 'A', 'B', 'B-A'))
    for k in keys:
        print('  %-12s %8.2f %8.2f %+8.2f' % (k, agg['A'][k], agg['B'][k], agg['B'][k] - agg['A'][k]))
    print('A = %s\nB = %s' % (a_elf, b_elf))
    hot = sorted(w, key=lambda o: -w[o])[:top]
    print('\nhottest groups:  share | A: insn ld-stack st-stack ld-lit ld-other | B: same')
    for op in hot:
        sa, sb = stats(A[op]), stats(B[op])
        f = lambda s: '%3d %2d %2d %2d %2d' % (s['insn'], s['ld stack'], s['st stack'], s['ld literal'], s['ld other'])
        print('  %02x  %5.2f%%  | %s | %s' % (op, 100.0 * w[op] / tot, f(sa), f(sb)))
    for op in hot[:ndiff]:
        na = [norm(mn, args) for _, _, mn, args in A[op]]
        nb = [norm(mn, args) for _, _, mn, args in B[op]]
        print('\n=== group %02x (%.2f%%): A %d insn, B %d insn' % (op, 100.0 * w[op] / tot, len(na), len(nb)))
        for l in difflib.unified_diff(na, nb, 'A', 'B', lineterm='', n=1):
            print('  ' + l)

if __name__ == '__main__':
    main()
