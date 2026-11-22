"""gba_hle_armgen.py -- ARM (guest) code to exact native C, for the game HLE
generators (tools/gba_hle_mgolf_gen.py, tools/gba_hle_wcp_gen.py).

decode() writes one guest instruction as a C statement over locals r0-r14 and
the flags n, z, c, v, with the interpreter's memory accesses and cycle
charges (cpu.cc; the rules in tools/gba_hle_vrally3_gen.py): the caller
charges each instruction's fetch F. The macros it uses (LDSB, LDB, STB, STH,
ST_OK, BLK_OK, EXIT, EXIT_DONE) are the routine's. live_flags() is the flag
liveness the statements need (a flag is set only if read, or held at an exit
point, before the next certain write); cost()/seg_worst() the most a run of
instructions can cost, for the checkpoints.
"""

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


# STM into VRAM through write_memory32 (a routine's generator sets it)
VRAM_STM = False

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
    transfer, BX) or at it (a branch)"""
    top = (op >> 25) & 7
    if (op & 0x0FFFFFF0) == 0x012FFF10:
        return True
    if (op & 0x0E000090) == 0x00000090 and (op >> 5) & 3:     # halfword: a store
        return not (op >> 20) & 1
    return top == 4 or (top in (2, 3) and not (op >> 20) & 1) or top == 5


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


def decode(pc, op, in_range, label, dyn_branch=None, live='nzcv', rem=None, ret_targets=()):
    """-> (statement, charges done inside). The fetch F is added by STEP."""
    cond = op >> 28
    assert cond != 0xF
    cnd = COND[cond]
    nxt = pc + 4
    top = (op >> 25) & 7
    if top == 5 and (op & (1 << 24)):            # BL: in the routine only
        tgt = b_target(pc, op)
        assert in_range(tgt) and rem is not None, 'BL out of the routine'
        return ('if (%s) { r14 = 0x%08Xu; cyc -= BT; cyc += %d * F; goto %s; }'
                % (cnd, pc + 4, rem - 1, label(tgt)))
    if top == 5:                                  # B
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
            if in_range(tgt):   # to a checkpoint of the routine's
                return 'if (%s) { cyc -= BT; cyc += %d * F; goto %s; }' % (cnd, rem - 1, label(tgt))
            return 'if (%s) { cyc -= BT; cyc += %d * F; EXIT_DONE(0x%08Xu); }' % (cnd, rem - 1, tgt)
        if dyn_branch:
            return 'if (%s) { cyc -= BT + F; %s }' % (cnd, dyn_branch)
        return 'if (%s) { cyc -= BT + F; if (cyc <= 0) EXIT_DONE(0x%08Xu); %s }' % (cnd, tgt, go)
    if (op & 0x0FFFFFF0) == 0x012FFF10:           # BX
        assert rem is not None
        rm = reg(op & 0xF)
        if not ret_targets:     # the interpreter does it
            return 'if (%s) { cyc += %d * F; EXIT(0x%08Xu); }' % (cnd, rem, pc)
        # to one of the routine's return sites (ARM): there; else the interpreter
        sw = ' '.join('case 0x%08Xu: goto %s;' % (t, label(t)) for t in sorted(ret_targets))
        return ('if (%s) { cyc -= BT; cyc += %d * F; switch (%s) { %s } cyc += BT + F; EXIT(0x%08Xu); }'
                % (cnd, rem - 1, rm, sw, pc))
    if top == 4:                                  # LDM/STM
        p, u, sbit, w, l = [(op >> k) & 1 for k in (24, 23, 22, 21, 20)]
        assert (p, u, sbit) in ((0, 1, 0), (1, 0, 0)), 'LDM/STM form'
        assert (p, u) == (0, 1) or (w and not l), 'STMDB rn! only'
        rn, lst = (op >> 16) & 0xF, op & 0xFFFF
        assert not (lst & (1 << rn)) and not (lst & 0x8000)
        regs = [R[i] for i in range(16) if lst >> i & 1]
        nops = len(regs)
        if p:                   # STMDB rn!: the words from rn - 4n up, rn = that
            body = '{ u32 _a = (%s - %du) & ~3u; ' % (R[rn], 4 * nops)
            body += 'if (!BLK_OK(_a, %d)) %s ' % (4 * nops, ex_store(pc, rem))
            body += 'cyc -= %d * ws_cyc_seq[_a >> 24][1]; ' % nops
            body += 'u8 *_p = memory_map_read[_a >> 15] + (_a & 0x7FFF); '
            body += ' '.join('address32(_p, %d) = %s;' % (4 * k, r) for k, r in enumerate(regs)) + ' '
            body += 'gba_idle_sidefx += %d; %s = %s - %du; }' % (nops, R[rn], R[rn], 4 * nops)
            return 'if (%s) %s' % (cnd, body)
        body = '{ u32 _a = %s & ~3u; ' % R[rn]
        if not l and VRAM_STM:
            # into VRAM: the interpreter's general path (store_aligned32 ->
            # write_memory32 per word, one more side effect a transfer)
            body += 'if (BLK_VRAM(_a, %d)) { cyc -= %d * ws_cyc_seq[6][1]; ' % (4 * nops, nops)
            body += 'const u32 _v[%d] = { %s }; VRAM_STM(_a, _v, %d); ' % (nops, ', '.join(regs), nops)
            body += 'gba_idle_sidefx += %d;%s } else { ' % (
                nops + 1, (' %s = %s + %du;' % (R[rn], R[rn], 4 * nops)) if w else '')
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
        if not l and VRAM_STM:
            body += ' }'
        return 'if (%s) %s' % (cnd, body)
    if (op & 0x0E000090) == 0x00000090 and (op >> 5) & 3:     # halfword/signed transfer
        p, u, i, w, l = [(op >> k) & 1 for k in (24, 23, 22, 21, 20)]
        sh = (op >> 5) & 3
        assert l or sh == 1, 'only LDRSB, LDRH, LDRSH, STRH'
        rn, rd = (op >> 16) & 0xF, (op >> 12) & 0xF
        off = '%du' % (((op >> 4) & 0xF0) | (op & 0xF)) if i else reg(op & 0xF)
        sgn = '+' if u else '-'
        if not l:               # STRH: work RAM, else stop before it
            assert p and not w, 'STRH form'
            body = ('{ u32 _v = %s; u32 _a = (%s %s %s) & ~1u; if (!ST_OK(_a)) %s STH(_a, _v); }'
                    % (reg(rd), reg(rn), sgn, off, ex_store(pc, rem)))
            return 'if (%s) %s' % (cnd, body)
        if p:
            body = '{ u32 _a = %s %s %s; ' % (reg(rn), sgn, off)
            if w:
                body += '%s = _a; ' % R[rn]
        else:
            body = '{ u32 _a = %s; %s = %s %s %s; ' % (reg(rn), R[rn], R[rn], sgn, off)
        body += '%s = %s(_a); }' % (R[rd], {2: 'LDSB', 1: 'LDH', 3: 'LDSH'}[sh])
        return 'if (%s) %s' % (cnd, body)
    if (op & 0x0FC000F0) == 0x00000090:          # MUL/MLA
        a, s = (op >> 21) & 1, (op >> 20) & 1
        assert not s
        rd, rn, rs, rm = (op >> 16) & 0xF, (op >> 12) & 0xF, (op >> 8) & 0xF, op & 0xF
        e = '%s * %s' % (reg(rm), reg(rs)) + (' + %s' % reg(rn) if a else '')
        return 'if (%s) %s = %s;' % (cnd, R[rd], e)
    if top in (2, 3):                             # LDR/STR
        i, p, u, b, w, l = [(op >> k) & 1 for k in (25, 24, 23, 22, 21, 20)]
        if not b:               # LDR / STR word, an immediate offset
            assert not i and not (p and w), 'LDR/STR form'
            rn, rd = (op >> 16) & 0xF, (op >> 12) & 0xF
            assert rd != 15
            off = op & 0xFFF
            sgn = '+' if u else '-'
            if rn == 15:        # a literal: the address is a constant
                assert p and l
                return 'if (%s) %s = LDW(0x%08Xu);' % (cnd, R[rd], pc + 8 + (off if u else -off))
            if p:
                a = '%s %s %du' % (reg(rn), sgn, off)
                if l:
                    return 'if (%s) %s = LDW(%s);' % (cnd, R[rd], a)
                return ('if (%s) { u32 _v = %s; u32 _a = (%s) & ~3u; if (!ST_OK(_a)) %s STW(_a, _v); }'
                        % (cnd, reg(rd), a, ex_store(pc, rem)))
            # post-index: the access at rn, then rn += off
            if l:
                return ('if (%s) { u32 _a = %s; %s = %s %s %du; %s = LDW(_a); }'
                        % (cnd, reg(rn), R[rn], R[rn], sgn, off, R[rd]))
            return ('if (%s) { u32 _v = %s; u32 _a = %s & ~3u; if (!ST_OK(_a)) %s %s = %s %s %du; STW(_a, _v); }'
                    % (cnd, reg(rd), reg(rn), ex_store(pc, rem), R[rn], R[rn], sgn, off))
        rn, rd = (op >> 16) & 0xF, (op >> 12) & 0xF
        if i:
            assert not (op & 0xFF0), 'shifted register offset'
            off = reg(op & 0xF)
        else:
            off = '%du' % (op & 0xFFF)
        sgn = '+' if u else '-'
        if l:                   # LDRB
            assert not w, 'LDRB form'
            if p:
                return 'if (%s) %s = LDB(%s %s %s);' % (cnd, R[rd], reg(rn), sgn, off)
            return ('if (%s) { u32 _a = %s; %s = %s %s %s; %s = LDB(_a); }'
                    % (cnd, reg(rn), R[rn], R[rn], sgn, off, R[rd]))
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


def fnv(words):
    h = 2166136261
    for x in words:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def cost(op, back=False):
    """(fetches, BT, byte accesses, block words) of one instruction; a single
    word access counts as a block word (its bound uses the slower of them)"""
    top = (op >> 25) & 7
    if top in (2, 3) and not (op >> 22) & 1:
        return (1, 0, 0, 1)
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


