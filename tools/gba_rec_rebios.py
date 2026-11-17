#!/usr/bin/env python3
"""gba_rec_rebios.py -- carry recordings over to a changed bundled BIOS.

  tools/gba_rec_rebios.py <old_bios.bin> <new_bios.bin> <rec> [<rec> ...]

A recording (src/porting/gba_rec.c) stores a setup hash: FNV-1a of the BIOS
image (main_gba.c gba_setup_hash), extended with the text of every cheat that
was enabled. A recording made with the old bundled BIOS and no cheats has
exactly FNV(old image) there: this rewrites it to FNV(new image), in place, and
touches nothing else. Any other value (cheats on, or a real BIOS in
/bios/gba/) is reported and left alone.

Only valid for a game that does not use what changed between the two images:
check it with the host replay first (GBA_REC_ANY_SETUP=1 replays a recording
despite the mismatch; it must print the same REC/hash lines as the recording).

The old image of the 2026-11-17 fix (nine rebased addresses):
  git show f97365a^:src/gpsp/bios/open_gba_bios.bin > old_bios.bin
"""
import struct
import sys

REC_MAGIC = 0x43524247   # "GBRC" (gba_rec.c REC_MAGIC), version 1
SETUP_OFF = 12   # rec_header_t: magic u32, version u16, flags u16, rom_hash u32, setup_hash u32


def fnv(data, h=2166136261):
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    old = fnv(open(sys.argv[1], 'rb').read())
    new = fnv(open(sys.argv[2], 'rb').read())
    print('old BIOS setup hash %08x, new %08x' % (old, new))
    for path in sys.argv[3:]:
        with open(path, 'r+b') as f:
            head = f.read(SETUP_OFF + 4)
            if len(head) < SETUP_OFF + 4 or struct.unpack_from('<IH', head, 0) != (REC_MAGIC, 1):
                print('%s: not a version 1 recording' % path)
                continue
            setup = struct.unpack_from('<I', head, SETUP_OFF)[0]
            if setup == new:
                print('%s: already the new BIOS' % path)
            elif setup == old:
                f.seek(SETUP_OFF)
                f.write(struct.pack('<I', new))
                print('%s: rewritten for the new BIOS' % path)
            else:
                print('%s: setup %08x (cheats, or another BIOS): left alone' % (path, setup))


if __name__ == '__main__':
    main()
