#!/bin/bash
# gba_bios_census.sh -- how much of each game's interpreted work is BIOS code
# (and how many SWIs a frame enter the BIOS instead of the native BIOS
# functions): which games a change to the BIOS paths would reach.
#
#   tools/gba_bios_census.sh <rom dir> [frames] [jobs] > out.txt
#
# Per game: a host run from power-on with no input (GBA_RECORD=1 into a
# temporary directory: nothing is written next to the ROMs), the PC histogram
# over frames 600..<frames>; prints "<game>|<BIOS %>|<instr/frame>|<BIOS SWI
# entries/frame>", sorted by BIOS share. GBA_CENSUS_TIMEOUT (default 40 s)
# per run.
set -u
ROMS=$(readlink -f "${1:?rom dir}"); FRAMES=${2:-1800}; JOBS=${3:-6}
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
export HERE T FRAMES
one() {
  local rom="$1" n d out every
  n=$(basename "$rom" .gba); d="$T/$(echo "$n" | tr -c 'A-Za-z0-9\n' _)"
  mkdir -p "$d"; ln -sf "$rom" "$d/g.gba"
  every=$((FRAMES - 600))
  out=$(cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 GBA_RECORD=1 \
        GBA_PCHIST=1 GBA_PCHIST_FROM=600 GBA_PCHIST_EVERY=$every GBA_PCHIST_TOP=300 \
        GBA_EXIT_FRAME=$((FRAMES + 2)) timeout "${GBA_CENSUS_TIMEOUT:-40}" "$HERE/gba_host" g.gba 2>/dev/null |
        grep -a "^PCHIST frames\|^PCHIST pc=00000008 ")
  local bios instr swi
  bios=$(echo "$out" | sed -n 's/.*bios=\([0-9.]*\)%.*/\1/p' | head -1)
  instr=$(echo "$out" | sed -n 's/.*instr=\([0-9]*\) per frame.*/\1/p' | head -1)
  swi=$(echo "$out" | sed -n 's/^PCHIST pc=00000008 A n=\([0-9]*\).*/\1/p' | head -1)
  echo "$n|${bios:-?}|${instr:-?}|$(( ${swi:-0} / every ))"
}
export -f one
find "$ROMS" -maxdepth 1 -name '*.gba' -print0 | xargs -0 -P "$JOBS" -I{} bash -c 'one "$1"' _ {} |
  sort -t'|' -k2 -g -r
rm -rf "$T"
