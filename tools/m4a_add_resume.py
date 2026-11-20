#!/usr/bin/env python3
"""m4a_add_resume.py -- give every M4A mixer runner (src/m4a/m4a_hle.c) an
entry at each of its instructions, so a block that gave way to an interrupt
can be resumed natively where the interrupt returns (m4a_gpsp.c).

  tools/m4a_add_resume.py src/m4a/m4a_hle.c        (rewrites it in place)

Per `static int m4a_run_*` function:
  - each instruction line (`/* 30xxxxx: ...` comment, one per guest
    instruction) gets a label R<offset>, the offset from the block's first
    instruction (variants 1 and 2 quote absolute addresses, the others
    offsets);
  - after the flags are loaded, `if (s->resume)` skips the entry checks (the
    block was validated when it started) and jumps, after the `#define
    FAILED()` line, through a switch on s->resume to the label.
Idempotent: a file that already has the labels is left as it is.
"""
import re
import sys

path = sys.argv[1]
src = open(path).read()
if 'goto m4a_resume_at;' in src:
    print('already has resume entries')
    sys.exit(0)
lines = src.split('\n')
out = []
i = 0
INSN = re.compile(r'^(\s*)(/\* 30([0-9a-f]{5}):)')
while i < len(lines):
    line = lines[i]
    if not line.startswith('static int m4a_run_'):
        out.append(line)
        i += 1
        continue
    # the whole function: up to the line `}` at column 0
    j = i
    while lines[j] != '}':
        j += 1
    body = lines[i:j + 1]
    addrs = [int(m[3], 16) for m in (INSN.match(l) for l in body) if m]
    first = min(addrs)
    offs = []
    nb = []
    for l in body:
        m = INSN.match(l)
        if m:
            off = int(m[3], 16) - first
            offs.append(off)
            l = '%sR%03x: ;%s' % (m[1], off, l[len(m[1]):])
        nb.append(l)
    body = nb
    # the resume jump, after the flags load
    k = next(n for n, l in enumerate(body) if l.strip().startswith('n_f = s->n;'))
    body.insert(k + 1, '\n    if (s->resume) {               /* validated when the block started */\n'
                       '        win_init(&wch,  bus);\n        win_init(&wmix, bus);\n'
                       '        win_init(&wsmp, bus);\n        win_init(&wstk, bus);\n'
                       '        goto m4a_resume_at;\n    }')
    k = next(n for n, l in enumerate(body) if l.startswith('#define FAILED()'))
    cases = [o for o in offs if o]
    sw = ['', '    if (0) {', 'm4a_resume_at:', '        switch (s->resume) {']
    for n in range(0, len(cases), 6):
        sw.append('        ' + ' '.join('case 0x%03xu: goto R%03x;' % (o, o) for o in cases[n:n + 6]))
    sw += ['        default: return M4A_DECLINED;', '        }', '    }']
    body[k + 1:k + 1] = sw
    out.extend(body)
    i = j + 1
open(path, 'w').write('\n'.join(out))
print('resume entries added')
