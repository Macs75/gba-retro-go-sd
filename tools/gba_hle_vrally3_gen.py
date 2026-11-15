#!/usr/bin/env python3
"""gba_hle_vrally3_gen.py -- generate V-Rally 3's native rasterizer routine.

  tools/gba_hle_vrally3_gen.py > src/hle/hle_vrally3.inc

V-Rally 3 (AVRE) draws its textured polygons with one ARM routine in IWRAM,
0x03003AB0-0x03003D04 (150 instructions, ~40% of the race's guest
instructions once its span loop ran natively). This writes it as C, one
labelled statement per guest instruction, each followed by its exact cycle
charge and the interpreter's own stop test (cycles <= 0 after an
instruction: stop with the PC on the next one). The routine can be entered
at any of its instructions (a jump lands anywhere in it: the interpreter
then calls it, src/hle/hle_vrally3.cc), and leaves at 0x03003C64 (the epilogue, interpreted)
or when the cycles run out, with the exact state the interpreter would have.

Cycle rules (cpu.cc): every instruction pays the fetch at its next PC,
ws_cyc_seq[region][1] (IWRAM here: F); a taken branch B also pays
ws_cyc_nseq[target region][1] (BT); a failed condition only F; LDR/LDRB pay
ws_cyc_nseq[region][1/0] of the address, STR ws_cyc_nseq[region][1]; a
one-register LDM/STM pays ws_cyc_seq[region][1]; MUL, shifts and writes to
the PC (the computed jumps) nothing more.

The instruction table below is the routine as the game copies it to IWRAM
(the opcodes are checked by their hash before the native code runs).
"""

BASE = 0x03003AB0
END = 0x03003D04

prog = {}   # pc -> ('kind', ...)

def op(pc, *a):
    prog[pc] = a

def texel(pc, shift, into_r9=False, add_r6=True):
    """and lr, r6, #0xff00 ; add lr, lr, fp ; ldrb X, [lr, r6, lsr #24] ;
    [orr sb, sb, lr, lsl #shift] ; [add r6, r6, r2]"""
    op(pc, 'c', 'r14 = r6 & 0xff00u;')
    op(pc + 4, 'c', 'r14 = r14 + r11;')
    if into_r9:
        op(pc + 8, 'ld8', 'r9', 'r14 + (r6 >> 24)')
        nxt = pc + 12
    else:
        op(pc + 8, 'ld8', 'r14', 'r14 + (r6 >> 24)')
        op(pc + 12, 'c', 'r9 = r9 | (r14 << %d);' % shift if shift else 'r9 = r9 | r14;')
        nxt = pc + 16
    if add_r6:
        op(nxt, 'c', 'r6 = r6 + r2;')
        nxt += 4
    return nxt

# per-line setup
op(0x03003AB0, 'c', 'r4 = r7 >> 24;')
op(0x03003AB4, 'c', 'r4 = r4 - (r8 >> 24);')
op(0x03003AB8, 'ld32', 'r2', '0x03003A80u')
op(0x03003ABC, 'ld32', 'r2', 'r2 + (r4 << 2)')
op(0x03003AC0, 'c', 'r3 = r1 >> 20;')
op(0x03003AC4, 'c', 'r3 = r3 - (r0 >> 20);')
op(0x03003AC8, 'c', 'r3 = r2 * r3;')
op(0x03003ACC, 'c', 'r3 = (u32)((s32)r3 >> 15);')
op(0x03003AD0, 'c', 'r6 = r1 << 16;')
op(0x03003AD4, 'c', 'r6 = r6 >> 20;')
op(0x03003AD8, 'c', 'r9 = r0 << 16;')
op(0x03003ADC, 'c', 'r6 = r6 - (r9 >> 20);')
op(0x03003AE0, 'c', 'r6 = r2 * r6;')
op(0x03003AE4, 'c', 'r6 = (u32)((s32)r6 >> 15);')
op(0x03003AE8, 'c', 'r2 = r6 + (r3 << 16);')
op(0x03003AEC, 'c', 'r6 = r0;')
op(0x03003AF0, 'c', 'r3 = r10 + (r8 >> 24);')
op(0x03003AF4, 'c', 'r3 = r3 & ~3u;')
op(0x03003AF8, 'c', 'r12 = r8 >> 24;')
op(0x03003AFC, 'c', 'r12 = r12 & 3u;')
op(0x03003B00, 'subs', 'r9', 'r4', '4u')
op(0x03003B04, 'b', 'n', 0x03003C74)
# head: the first, partial word
op(0x03003B08, 'c', 'r4 = r9 + r12;')
op(0x03003B0C, 'c', 'r9 = r12 << 3;')
op(0x03003B10, 'c', 'r12 = 0x20u - r9;')
op(0x03003B14, 'ld32', 'r9', 'r3')
op(0x03003B18, 'c', 'r9 = LSLR(r9, r12);')
op(0x03003B1C, 'c', 'r9 = LSRR(r9, r12);')
op(0x03003B20, 'c', 'r14 = 0x03003B7Cu;')            # add lr, pc, #0x54
op(0x03003B24, 'c', 'r14 = r14 - (r12 << 1);')
op(0x03003B28, 'jmp', 'r14 - (r12 >> 1)')
p = texel(0x03003B2C, 0)
p = texel(p, 8)
p = texel(p, 16)
p = texel(p, 24)
assert p == 0x03003B7C
op(0x03003B7C, 'strwb', 'r3', 'r9')
op(0x03003B80, 'subs', 'r4', 'r4', '4u')
op(0x03003B84, 'b', 'n', 0x03003BE0)
# the 4-texel loop
p = texel(0x03003B88, 0, into_r9=True)
p = texel(p, 8)
p = texel(p, 16)
p = texel(p, 24)
assert p == 0x03003BD4
op(0x03003BD4, 'strwb', 'r3', 'r9')
op(0x03003BD8, 'subs', 'r4', 'r4', '4u')
op(0x03003BDC, 'b', '!n', 0x03003B88)
# tail: the last, partial word
op(0x03003BE0, 'c', 'r12 = r4 & 3u;')
op(0x03003BE4, 'c', 'r12 = r12 << 3;')
op(0x03003BE8, 'ld32', 'r9', 'r3')
op(0x03003BEC, 'c', 'r9 = LSRR(r9, r12);')
op(0x03003BF0, 'c', 'r14 = 0x03003C38u;')            # add lr, pc, #0x40
op(0x03003BF4, 'c', 'r14 = r14 - (r12 << 1);')
op(0x03003BF8, 'jmp', 'r14 - (r12 >> 1)')
p = texel(0x03003BFC, 8)
p = texel(p, 16)
p = texel(p, 24, add_r6=False)
assert p == 0x03003C34
op(0x03003C34, 'c', 'r12 = 0x20u - r12;')
op(0x03003C38, 'c', 'r9 = RORR(r9, r12);')
op(0x03003C3C, 'str', 'r3', 'r9')
# next line
op(0x03003C40, 'ld32', 'r2', '0x03003A9Cu')
op(0x03003C44, 'c', 'r0 = r0 + r2;')
op(0x03003C48, 'ld32', 'r2', '0x03003A94u')
op(0x03003C4C, 'c', 'r1 = r1 + r2;')
op(0x03003C50, 'c', 'r7 = r7 + (r7 << 16);')
op(0x03003C54, 'c', 'r8 = r8 + (r8 << 16);')
op(0x03003C58, 'c', 'r10 = r10 + 0xf0u;')
op(0x03003C5C, 'subs', 'r5', 'r5', '1u')
op(0x03003C60, 'b', '!z', 0x03003AB0)
for pc in (0x03003C64, 0x03003C68, 0x03003C6C, 0x03003C70):
    op(pc, 'exit')                                     # the epilogue: interpreted
# a span shorter than a word
op(0x03003C74, 'cmp', 'r4', '0u')
op(0x03003C78, 'b', '(z || n != v)', 0x03003C40)
op(0x03003C7C, 'c', 'r14 = 4u - r12;')
op(0x03003C80, 'cmp', 'r14', 'r4')
op(0x03003C84, 'c', 'if (!z && n == v) r14 = r4;')  # movgt lr, r4
op(0x03003C88, 'subs', 'r4', 'r4', 'r14')
op(0x03003C8C, 'c', 'r12 = r12 << 3;')
op(0x03003C90, 'c', 'r14 = r14 << 3;')
op(0x03003C94, 'ld32', 'r9', 'r3')
op(0x03003C98, 'c', 'r9 = RORR(r9, r12);')
op(0x03003C9C, 'c', 'r9 = LSRR(r9, r14);')
op(0x03003CA0, 'c', 'r12 = r12 + r14;')
op(0x03003CA4, 'c', 'r12 = 0x20u - r12;')
op(0x03003CA8, 'push', 'r12')                         # stmdb sp!, {ip}
op(0x03003CAC, 'c', 'r12 = 0x03003CF4u;')            # add ip, pc, #0x40
op(0x03003CB0, 'c', 'r12 = r12 - (r14 << 1);')
op(0x03003CB4, 'jmp', 'r12 - (r14 >> 1)')
p = texel(0x03003CB8, 8)
p = texel(p, 16)
p = texel(p, 24)
assert p == 0x03003CF4
op(0x03003CF4, 'pop', 'r12')                          # ldm sp!, {ip}
op(0x03003CF8, 'c', 'r9 = RORR(r9, r12);')
op(0x03003CFC, 'strwb', 'r3', 'r9')
op(0x03003D00, 'b', 'z', 0x03003C40)
op(0x03003D04, 'b', '1', 0x03003BE0)

assert sorted(prog) == list(range(BASE, END + 4, 4)), 'gap in the table'

def lab(pc):
    return 'L_%04x' % (pc & 0xFFFF)

out = []
w = out.append
w('/* Generated by tools/gba_hle_vrally3_gen.py: do not edit. */')
w('#define VR3_BASE 0x%08Xu' % BASE)
w('#define VR3_N    %du' % len(prog))
w('  static void *const vr3_tbl[VR3_N] = {   /* in the overlay, with the code */')
for pc in sorted(prog):
    w('    &&%s,' % lab(pc))
w('  };')
w('  goto *vr3_tbl[(pc - VR3_BASE) >> 2];')
for pc in sorted(prog):
    a = prog[pc]
    k = a[0]
    nxt = pc + 4
    if k == 'c':
        w('%s: %s STEP(0x%08Xu, F);' % (lab(pc), a[1], nxt))
    elif k == 'ld32':
        w('%s: { u32 _a = %s; %s = LD32(_a); } STEP(0x%08Xu, F);' % (lab(pc), a[2], a[1], nxt))
    elif k == 'ld8':
        w('%s: { u32 _a = %s; %s = LD8(_a); } STEP(0x%08Xu, F);' % (lab(pc), a[2], a[1], nxt))
    elif k in ('str', 'strwb'):
        wb = ' %s += 4;' % a[1] if k == 'strwb' else ''
        w('%s: { u32 _a = %s & ~3u; if (!ST_OK(_a)) EXIT(0x%08Xu);'
          ' ST32(_a, %s);%s } STEP(0x%08Xu, F);' % (lab(pc), a[1], pc, a[2], wb, nxt))
    elif k in ('subs', 'cmp'):
        if k == 'subs':
            d, s1, s2 = a[1], a[2], a[3]
        else:
            d, s1, s2 = None, a[1], a[2]
        body = '{ u32 _x = %s, _y = %s, _r = _x - _y; n = _r >> 31; z = _r == 0;' \
               ' c = _x >= _y; v = ((_x ^ _y) & (_x ^ _r)) >> 31;%s }' % (
                   s1, s2, ' %s = _r;' % d if d else '')
        w('%s: %s STEP(0x%08Xu, F);' % (lab(pc), body, nxt))
    elif k == 'b':
        cond, tgt = a[1], a[2]
        w('%s: if (%s) { cyc -= BT + F; if (cyc <= 0) EXIT(0x%08Xu); goto %s; }'
          ' STEP(0x%08Xu, F);' % (lab(pc), cond, tgt, lab(tgt), nxt))
    elif k == 'jmp':
        w('%s: { u32 _t = %s; cyc -= F; if (cyc <= 0) EXIT_DONE(_t);'
          ' if (_t - VR3_BASE >= VR3_N * 4u || (_t & 3)) EXIT_DONE(_t);'
          ' goto *vr3_tbl[(_t - VR3_BASE) >> 2]; }' % (lab(pc), a[1]))
    elif k == 'push':
        w('%s: { u32 _a = r13 - 4; if (((_a >> 24) - 2) > 1) EXIT(0x%08Xu);'
          ' cyc -= ws_cyc_seq[_a >> 24][1]; cpu_hle_store32(_a, %s); r13 = _a; } STEP(0x%08Xu, F);'
          % (lab(pc), pc, a[1], nxt))
    elif k == 'pop':
        w('%s: { u32 _a = r13; %s = POP32(_a); r13 = _a + 4; } STEP(0x%08Xu, F);'
          % (lab(pc), a[1], nxt))
    elif k == 'exit':
        w('%s: EXIT(0x%08Xu);' % (lab(pc), pc))
    else:
        raise SystemExit('kind ' + k)
print('\n'.join(out))
