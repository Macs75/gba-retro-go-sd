#!/usr/bin/env python3
"""gba_hle_mgolf_gen.py -- generate Mario Golf: Advance Tour's native mixer loops.

  tools/gba_hle_mgolf_gen.py <Mario Golf - Advance Tour (USA).gba> > src/hle/hle_mgolf.inc

Mario Golf: Advance Tour (BMGE) mixes its sound with its own routine, which
the game copies from the ROM (offset 0x550) to IWRAM 0x03000000 and then
rewrites as it plays:
  - 0x03000A28-0x03000B5C, the interpolating loop: 8 output samples a pass
    (two ldm/stm groups of 4), each sample's 6 step/load instructions one of
    three templates (0x03000B80, 0x03000B98, 0x03000BB0: the pitch step's
    range) the game writes into the 8 slots when the channel needs another
    one, noting which at 0x03000B7C;
  - 0x03000E30-0x03000EE4, the output stage: 2 samples a pass, the echo of
    the previous buffer, the 8-bit output and its 16-bit copy; its last
    instruction is patched (from 0x03000F08 or 0x03000F0C) to loop to
    0x03000E3C or to 0x03000E34.
Together ~30% of the game's instructions in its field (2026-11-21).

This decodes those ARM words (the ROM's, with the templates applied) into C:
passes of each loop, from its head, one statement per guest instruction,
each with its exact cycle charge, and before every 16th instruction a
checkpoint: the next 16 run only when the cycles left exceed the most they can
cost, so no stop (cycles <= 0 after an instruction) can fall among them;
otherwise it stops there, at an instruction boundary, and the interpreter
runs the rest of the pass and lands at the head again. The cycle rules are tools/gba_hle_vrally3_gen.py's (cpu.cc): every instruction pays the
fetch at its next PC (IWRAM: F); a taken branch also BT; a failed condition
only F; LDRSB/STRB ws_cyc_nseq[region][0] of the address; an LDM/STM of n
registers n * ws_cyc_seq[region][1]; MUL/MLA nothing more. Stores outside
work RAM stop before the store. The idle skip's side-effect count follows the
interpreter's (one a STRB, n an STM).
"""
import struct
import sys

ROM_BASE = 0x550                 # the ROM offset copied to 0x03000000
IW = 0x03000000

L1_LO, L1_HI = 0x03000A28, 0x03000B5C     # the interpolating loop
L1_EXIT = 0x03000B60
TPL = [0x03000B80, 0x03000B98, 0x03000BB0]
L2_LO, L2_HI = 0x03000E30, 0x03000EE4     # the output stage
L2_EXIT = 0x03000EE8
# the templates run natively: the third (a step of two samples or more) is
# 2-6% of the loop's entries, interpreted (ITCM room, 2026-11-21)
L1_NATIVE = (0, 1)

rom = open(sys.argv[1], 'rb').read()


def rw(a):
    o = ROM_BASE + (a - IW)
    return struct.unpack('<I', rom[o:o + 4])[0]


def slots():
    """the template slots: 4 x 2 groups of (2 + 4) words"""
    out = []
    for i in range(4):
        for half in (0, 0x98):
            b = 0x03000A30 + 0x24 * i + half
            out.append([b, b + 4, b + 0xC, b + 0x10, b + 0x14, b + 0x18])
    return out


def loop1_words(v):
    w = {a: rw(a) for a in range(L1_LO, L1_HI + 4, 4)}
    t = [rw(TPL[v] + 4 * k) for k in range(6)]
    for s in slots():
        for k, a in enumerate(s):
            w[a] = t[k]
    return w


COND = ['z', '!z', 'c', '!c', 'n', '!n', 'v', '!v', '(c && !z)', '(!c || z)',
        '(n == v)', '(n != v)', '(!z && n == v)', '(z || n != v)', '1']
R = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11',
     'r12', 'r13', 'r14', 'PC']


def reg(i):
    assert i != 15, 'PC operand'
    return R[i]


def shifter(op):
    """operand2 -> (C expression, carry-out C expression or None = c kept)"""
    if op & (1 << 25):
        imm, rot = op & 0xFF, ((op >> 8) & 0xF) * 2
        val = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
        return '0x%08Xu' % val, ('%du' % (val >> 31) if rot else None)
    assert not (op & 0x10), 'register-specified shift'
    rm, typ, amt = reg(op & 0xF), (op >> 5) & 3, (op >> 7) & 0x1F
    if typ == 0:
        if amt == 0:
            return rm, None
        return '(%s << %d)' % (rm, amt), '((%s >> %d) & 1u)' % (rm, 32 - amt)
    if typ == 1:
        if amt == 0:
            return '0u', '(%s >> 31)' % rm
        return '(%s >> %d)' % (rm, amt), '((%s >> %d) & 1u)' % (rm, amt - 1)
    if typ == 2:
        if amt == 0:
            return '(u32)((s32)%s >> 31)' % rm, '(%s >> 31)' % rm
        return '(u32)((s32)%s >> %d)' % (rm, amt), '((%s >> %d) & 1u)' % (rm, amt - 1)
    if amt == 0:
        return '((c << 31) | (%s >> 1))' % rm, '(%s & 1u)' % rm
    return '((%s >> %d) | (%s << %d))' % (rm, amt, rm, 32 - amt), '((%s >> %d) & 1u)' % (rm, amt - 1)


def dataproc(op, live='nzcv'):
    opc, s = (op >> 21) & 0xF, (op >> 20) & 1
    rn, rd = (op >> 16) & 0xF, (op >> 12) & 0xF
    assert rd != 15, 'write to PC'
    b, sc = shifter(op)
    logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
    if logical:
        a = None if opc in (13, 15) else reg(rn)
        expr = {0: '%s & _b', 1: '%s ^ _b', 8: '%s & _b', 9: '%s ^ _b', 12: '%s | _b',
                13: '_b', 14: '%s & ~_b', 15: '~_b'}[opc]
        expr = expr % a if '%s' in expr else expr
        out = '{ u32 _b = %s; u32 _r = %s;' % (b, expr)
        if s:
            fl = ''.join([' n = _r >> 31;' if 'n' in live else '', ' z = _r == 0;' if 'z' in live else ''])
            out += fl
            if sc is not None and 'c' in live:
                # the carry from the operand (before _r: rd may be rm)
                out = '{ u32 _sc = %s; u32 _b = %s; u32 _r = %s;%s c = _sc;' % (sc, b, expr, fl)
        if opc not in (8, 9):
            out += ' %s = _r;' % R[rd]
        return out + ' }'
    a = reg(rn)
    if opc in (2, 10):      # SUB, CMP
        x, y, cin = a, '_b', None
        kind = 'sub'
    elif opc == 3:          # RSB
        x, y, kind = '_b', a, 'sub'
    elif opc in (4, 11):    # ADD, CMN
        x, y, kind = a, '_b', 'add'
    else:
        raise SystemExit('opcode %d' % opc)
    out = '{ u32 _b = %s; u32 _x = %s, _y = %s; u32 _r = _x %s _y;' % (b, x, y, '-' if kind == 'sub' else '+')
    if s:
        fl = {'n': ' n = _r >> 31;', 'z': ' z = _r == 0;',
              'c': ' c = _x >= _y;' if kind == 'sub' else ' c = _r < _x;',
              'v': ' v = ((_x ^ _y) & (_x ^ _r)) >> 31;' if kind == 'sub' else ' v = (~(_x ^ _y) & (_x ^ _r)) >> 31;'}
        out += ''.join(fl[f] for f in 'nzcv' if f in live)
    if opc not in (10, 11):
        out += ' %s = _r;' % R[rd]
    return out + ' }'


def ex_store(pc, rem):
    """a store or block transfer the native code does not do: stop before
    it (the rest of its segment's fetches, charged at the checkpoint, given
    back)"""
    if rem is None:
        return 'EXIT(0x%08Xu);' % pc
    return '{ cyc += %d * F; EXIT(0x%08Xu); }' % (rem, pc)


# flags: what each instruction reads (its condition, RRX) and may write
CREAD = ['z', 'z', 'c', 'c', 'n', 'n', 'v', 'v', 'cz', 'cz', 'nv', 'nv', 'nzv', 'nzv', '']


def freads(op):
    r = set(CREAD[op >> 28])
    top = (op >> 25) & 7
    if top in (0, 1) and not (op & (1 << 25)) and not (op & 0x10) and ((op >> 5) & 3) == 3 and ((op >> 7) & 0x1F) == 0:
        r.add('c')
    return r


def fwrites(op):
    """flags an instruction writes for certain (unconditional, S bit)"""
    top = (op >> 25) & 7
    if top not in (0, 1) or (op & 0x0E000090) == 0x00000090 or op >> 28 != 0xE:
        return set()
    if not (op >> 20) & 1:
        return set()
    opc = (op >> 21) & 0xF
    if opc in (0, 1, 8, 9, 12, 13, 14, 15):
        _, sc = shifter(op)
        return {'n', 'z', 'c'} if sc is not None else {'n', 'z'}
    return {'n', 'z', 'c', 'v'}


def can_exit(op):
    """an instruction that can leave before it runs (a store, a block
    transfer) or at it (a branch out)"""
    top = (op >> 25) & 7
    return top == 4 or (top in (2, 3)) or top == 5


def live_flags(words, addrs, exit_before):
    """per address, the flags its result must set: read later, or held at
    an exit point (a checkpoint or an exit-capable instruction), before the
    next instruction that surely sets them; the pass's end is an exit"""
    out = {}
    for i, a in enumerate(addrs):
        op = words[a]
        need = set()
        for f in 'nzcv':
            for b in addrs[i + 1:]:
                if b in exit_before or can_exit(words[b]) or f in freads(words[b]):
                    need.add(f)
                    break
                if f in fwrites(words[b]):
                    break
            else:
                need.add(f)
        out[a] = ''.join(sorted(need))
    return out


def decode(pc, op, in_range, label, dyn_branch=None, live='nzcv', rem=None):
    """-> (statement, charges done inside). The fetch F is added by STEP."""
    cond = op >> 28
    assert cond != 0xF
    cnd = COND[cond]
    nxt = pc + 4
    top = (op >> 25) & 7
    if top == 5:                                  # B
        assert not (op & (1 << 24)), 'BL'
        off = op & 0xFFFFFF
        if off & 0x800000:
            off -= 1 << 24
        tgt = pc + 8 + off * 4
        if dyn_branch:
            go = dyn_branch
        elif in_range(tgt):
            go = 'goto %s;' % label(tgt)
        else:
            go = 'EXIT_DONE(0x%08Xu);' % tgt
        if rem is not None:     # its fetch and the rest of the segment's were charged
            assert not in_range(tgt)
            return 'if (%s) { cyc -= BT; cyc += %d * F; EXIT_DONE(0x%08Xu); }' % (cnd, rem - 1, tgt)
        if dyn_branch:
            return 'if (%s) { cyc -= BT + F; %s }' % (cnd, dyn_branch)
        return 'if (%s) { cyc -= BT + F; if (cyc <= 0) EXIT_DONE(0x%08Xu); %s }' % (cnd, tgt, go)
    if top == 4:                                  # LDM/STM
        p, u, sbit, w, l = [(op >> k) & 1 for k in (24, 23, 22, 21, 20)]
        assert (p, u, sbit) == (0, 1, 0), 'LDM/STM form'
        rn, lst = (op >> 16) & 0xF, op & 0xFFFF
        assert not (lst & (1 << rn)) and not (lst & 0x8000)
        regs = [R[i] for i in range(16) if lst >> i & 1]
        nops = len(regs)
        body = '{ u32 _a = %s & ~3u; ' % R[rn]
        # the loops' buffers are work RAM: a transfer elsewhere, or across a
        # page, stops before it (the interpreter does it)
        body += 'if (!BLK_OK(_a, %d)) %s ' % (4 * nops, ex_store(pc, rem))
        body += 'cyc -= %d * ws_cyc_seq[_a >> 24][1]; ' % nops
        body += 'u8 *_p = memory_map_read[_a >> 15] + (_a & 0x7FFF); '
        if l:
            body += ' '.join('%s = readaddress32(_p, %d);' % (r, 4 * k) for k, r in enumerate(regs)) + ' '
        else:
            body += ' '.join('address32(_p, %d) = %s;' % (4 * k, r) for k, r in enumerate(regs)) + ' '
            body += 'gba_idle_sidefx += %d; ' % nops
        if w:
            body += '%s = %s + %du; ' % (R[rn], R[rn], 4 * nops)
        body += '}'
        return 'if (%s) %s' % (cnd, body)
    if (op & 0x0E000090) == 0x00000090 and (op >> 5) & 3:     # halfword/signed transfer
        p, u, i, w, l = [(op >> k) & 1 for k in (24, 23, 22, 21, 20)]
        sh = (op >> 5) & 3
        assert l and sh == 2, 'only LDRSB'
        rn, rd = (op >> 16) & 0xF, (op >> 12) & 0xF
        off = '%du' % (((op >> 4) & 0xF0) | (op & 0xF)) if i else reg(op & 0xF)
        sgn = '+' if u else '-'
        if p:
            body = '{ u32 _a = %s %s %s; ' % (reg(rn), sgn, off)
            if w:
                body += '%s = _a; ' % R[rn]
        else:
            body = '{ u32 _a = %s; %s = %s %s %s; ' % (reg(rn), R[rn], R[rn], sgn, off)
        body += '%s = LDSB(_a); }' % R[rd]
        return 'if (%s) %s' % (cnd, body)
    if (op & 0x0FC000F0) == 0x00000090:          # MUL/MLA
        a, s = (op >> 21) & 1, (op >> 20) & 1
        assert not s
        rd, rn, rs, rm = (op >> 16) & 0xF, (op >> 12) & 0xF, (op >> 8) & 0xF, op & 0xF
        e = '%s * %s' % (reg(rm), reg(rs)) + (' + %s' % reg(rn) if a else '')
        return 'if (%s) %s = %s;' % (cnd, R[rd], e)
    if top in (2, 3):                             # LDR/STR
        i, p, u, b, w, l = [(op >> k) & 1 for k in (25, 24, 23, 22, 21, 20)]
        assert b and not l, 'only STRB'
        rn, rd = (op >> 16) & 0xF, (op >> 12) & 0xF
        if i:
            assert not (op & 0xFF0), 'shifted register offset'
            off = reg(op & 0xF)
        else:
            off = '%du' % (op & 0xFFF)
        sgn = '+' if u else '-'
        if p:
            body = '{ u32 _v = %s; u32 _a = %s %s %s; if (!ST_OK(_a)) %s ' % (reg(rd), reg(rn), sgn, off, ex_store(pc, rem))
            if w:
                body += '%s = _a; ' % R[rn]
        else:
            body = ('{ u32 _v = %s; u32 _a = %s; if (!ST_OK(_a)) %s %s = %s %s %s; '
                    % (reg(rd), reg(rn), ex_store(pc, rem), R[rn], R[rn], sgn, off))
        body += 'STB(_a, _v); }'
        return 'if (%s) %s' % (cnd, body)
    if top in (0, 1):
        return 'if (%s) %s' % (cnd, dataproc(op, live))
    raise SystemExit('unknown op %08x at %08x' % (op, pc))


# ---- emit ----
out = []
w = out.append
w('/* Generated by tools/gba_hle_mgolf_gen.py: do not edit. */')
w('#ifndef MG_CODE')
w('#define MG_L1_LO 0x%08Xu' % L1_LO)
w('#define MG_L2_LO 0x%08Xu' % L2_LO)
# the words the native code relies on, checked at entry (mg_check)
t = [[rw(TPL[v] + 4 * k) for k in range(6)] for v in range(3)]
w('static const u32 mg_tpl[3][6] = {')
for v in range(3):
    w('  { %s },' % ', '.join('0x%08Xu' % x for x in t[v]))
w('};')
def b_target(pc, op):
    off = op & 0xFFFFFF
    if off & 0x800000:
        off -= 1 << 24
    return pc + 8 + off * 4


def fnv(words):
    h = 2166136261
    for x in words:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


BACK_A, BACK_B = rw(0x03000F08), rw(0x03000F0C)   # the two words patched in at L2_HI
TGT_A, TGT_B = b_target(L2_HI, BACK_A), b_target(L2_HI, BACK_B)
assert (TGT_A, TGT_B) == (0x03000E3C, 0x03000E34), (hex(TGT_A), hex(TGT_B))
w('#define MG_L2_BACK_A 0x%08Xu   /* bgt 0x%08X */' % (BACK_A, TGT_A))
w('#define MG_L2_BACK_B 0x%08Xu   /* bgt 0x%08X */' % (BACK_B, TGT_B))
# the loops' words, hashed (FNV-1a over the words in address order): the
# interpolating loop's once per template, the output stage's without the
# patched back branch
w('static const u32 mg_l1_fnv[3] = { %s };' % ', '.join(
    '0x%08Xu' % fnv([loop1_words(v)[a] for a in range(L1_LO, L1_HI + 4, 4)]) for v in range(3)))
w('#define MG_L2_FNV 0x%08Xu' % fnv([rw(a) for a in range(L2_LO, L2_HI, 4)]))


def counts(words, lo, hi):
    """instructions, byte loads/stores, block-transfer words in [lo, hi]"""
    ni = nb = nw = 0
    for a in range(lo, hi + 4, 4):
        op = words[a]
        ni += 1
        top = (op >> 25) & 7
        if top == 4:
            nw += bin(op & 0xFFFF).count('1')
        elif (op & 0x0E000090) == 0x00000090 and (op >> 5) & 3:
            nb += 1
        elif top in (2, 3):
            nb += 1
    return ni, nb, nw


l2w = {a: rw(a) for a in range(L2_LO, L2_HI + 4, 4)}
l2w[L2_HI] = BACK_A     # the back branch's cost is its form's either way

# Checkpoints: before every K-th instruction of a pass, the most the next
# K instructions can cost (fetches, the back branch if among them, each
# memory access at its buffer's wait state: B8 a byte, W32 a block word);
# with fewer cycles left than that, the pass stops there (exact state) and
# the interpreter runs the rest of it.
K = 16


def cost(op, back=False):
    """(fetches, BT, byte accesses, block words) of one instruction"""
    top = (op >> 25) & 7
    if top == 4:
        return (1, 0, 0, bin(op & 0xFFFF).count('1'))
    if (op & 0x0E000090) == 0x00000090 and (op >> 5) & 3:
        return (1, 0, 1, 0)
    if top in (2, 3):
        return (1, 0, 1, 0)
    return (1, 1 if back else 0, 0, 0)


def seg_worst(words, addrs, back_at):
    f = bt = b8 = w32 = 0
    for a in addrs:
        x = cost(words[a], a == back_at)
        f += x[0]; bt += x[1]; b8 += x[2]; w32 += x[3]
    return '(%d * F + %d * BT + %d * b8 + %d * w32)' % (f, bt, b8, w32)


def emit_body(words, addrs, back_at, skip_branch_at):
    """the statements of addrs (but the back branch), a checkpoint every K:
    the segment's fetches charged there, its other costs as they happen"""
    cps = set(addrs[k] for k in range(0, len(addrs), K))
    live = live_flags(words, addrs, cps)
    for k, a in enumerate(addrs):
        if k % K == 0:
            seg = addrs[k:k + K]
            nf = sum(1 for x in seg if x != skip_branch_at)
            w('  if (cyc <= %s) EXIT(0x%08Xu);' % (seg_worst(words, seg, back_at), a))
            w('  cyc -= %d * F;' % nf)
            seg_end = k + len(seg)
        if a == skip_branch_at:
            continue
        rem = sum(1 for x in addrs[k:seg_end] if x != skip_branch_at)
        w('  %s' % decode(a, words[a], lambda t: False, nolab, live=live[a], rem=rem))


w('#define MG_L1_NATIVE_MASK 0x%Xu' % sum(1 << v for v in L1_NATIVE))
w('#else')
nolab = lambda a: None
for v in L1_NATIVE:
    words = loop1_words(v)
    op = words[L1_HI]
    assert b_target(L1_HI, op) == L1_LO and op >> 28 == 0xC
    w('l1_v%d:' % v)
    emit_body(words, list(range(L1_LO, L1_HI + 4, 4)), L1_HI, L1_HI)
    w('  if (!z && n == v) { cyc -= BT + F; if (cyc <= 0) EXIT_DONE(0x%08Xu); goto l1_v%d; }' % (L1_LO, v))
    w('  cyc -= F;')
    w('  EXIT(0x%08Xu);' % L1_EXIT)
w('l2_b:')
w('  MG_TRACE_PT(0x%08Xu);' % TGT_B)
emit_body(l2w, list(range(TGT_B, TGT_A, 4)), None, None)
w('  goto l2_body;')
w('l2_a:')
w('  MG_TRACE_PT(0x%08Xu);' % TGT_A)
w('l2_body:')
emit_body(l2w, list(range(TGT_A, L2_HI + 4, 4)), L2_HI, L2_HI)
w('  if (!z && n == v) { cyc -= BT + F; if (l2_back_b) { if (cyc <= 0) EXIT_DONE(0x%08Xu); goto l2_b; }'
  ' if (cyc <= 0) EXIT_DONE(0x%08Xu); goto l2_a; }' % (TGT_B, TGT_A))
w('  cyc -= F;')
w('  EXIT(0x%08Xu);' % L2_EXIT)
w('#endif')
print('\n'.join(out))
