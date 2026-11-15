#!/bin/bash
# gba_ophist_census.sh -- executed ARM / Thumb opcode-group counts over a set
# of games, to tell the interpreter's rare cases from its hot ones.
#
#   tools/gba_ophist_census.sh <ophist host binary> <rom dir> <frames> <out dir> [jobs]
#
# The binary: make host HOST_EXTRA_DEFS="-DGBA_ARM_OPHIST -DGBA_THUMB_OPHIST"
# (touch src/gpsp/cpu.cc first), copied somewhere. Per game, one run from
# power-on with no input; its ARMOP/THOP lines (stderr, at exit) go to
# <out dir>/<game>.txt. Sum them with tools/gba_ophist_sum.py.
set -u
BIN=$(readlink -f "${1:?ophist binary}"); ROMS=$(readlink -f "${2:?rom dir}")
FRAMES=${3:?frames}; OUT=$(readlink -f "${4:?out dir}"); JOBS=${5:-6}
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 GBA_EXIT_FRAME="$FRAMES" BIN OUT
one() {
  local rom="$1" n d
  n=$(basename "$rom" .gba | tr -c 'A-Za-z0-9\n' _)
  d=$(mktemp -d)
  ln -sf "$rom" "$d/g.gba"
  (cd "$d" && timeout "${GBA_CENSUS_TIMEOUT:-90}" "$BIN" g.gba 2>&1 >/dev/null | grep -aE '^(ARMOP|THOP) ' > "$OUT/$n.txt")
  rm -f "$d/g.gba"; rmdir "$d" 2>/dev/null
}
export -f one
find "$ROMS" -maxdepth 1 -name '*.gba' -print0 | xargs -0 -P "$JOBS" -I{} bash -c 'one "$@"' _ {}
echo "done: $(ls "$OUT" | wc -l) games in $OUT"
