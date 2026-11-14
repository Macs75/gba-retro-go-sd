#!/bin/bash
# gba_idle_ab.sh -- the generic idle skip (src/porting/gba_idle.c) against
# no skip, on every game it turns on for: same REC/hash lines or not, and
# what it removed.
#
#   tools/gba_idle_ab.sh <rom dir> <frames> [jobs]
#
# Per game, two host runs from power-on with no input (GBA_RECORD=1 into a
# temporary directory: nothing is written next to the ROMs), one with
# GBA_NOIDLE=1; a REC/hash line every 60 frames. Prints, per game the skip
# is on for: SAME / DIFFERS (first frame), instructions per frame without
# and with, cycles skipped per frame. GBA_AB_TIMEOUT (default 60 s) per run.
# GBA_AB_REF=<host binary>: the "off" run is that binary instead (idle skip
# on): a build against a reference build, e.g. an interpreter change.
# RTC carts (Pokemon Ruby/Sapphire/Emerald...) differ here because each run
# takes the PC's clock: A/B those by replaying one recording both ways.
set -u
ROMS=$(readlink -f "${1:?rom dir}"); FRAMES=${2:?frames}; JOBS=${3:-6}
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 GBA_RECORD=1 GBA_HASH_EVERY=60 GBA_EXIT_FRAME="$FRAMES"
one() {
  local rom="$1" n d
  n=$(basename "$rom" .gba); d="$T/$(echo "$n" | tr -c 'A-Za-z0-9\n' _)"
  mkdir -p "$d/on" "$d/off"
  ln -sf "$rom" "$d/on/g.gba"; ln -sf "$rom" "$d/off/g.gba"
  (cd "$d/on" && timeout "${GBA_AB_TIMEOUT:-60}" "$HERE/gba_host" g.gba > log.txt 2>&1)
  if [ -n "${GBA_AB_REF:-}" ]; then
    (cd "$d/off" && timeout "${GBA_AB_TIMEOUT:-60}" "$GBA_AB_REF" g.gba > log.txt 2>&1)
  else
    (cd "$d/off" && GBA_NOIDLE=1 timeout "${GBA_AB_TIMEOUT:-60}" "$HERE/gba_host" g.gba > log.txt 2>&1)
  fi
  local a b res
  a=$(grep -a "REC/hash" "$d/on/log.txt"); b=$(grep -a "REC/hash" "$d/off/log.txt")
  if [ -z "$a" ]; then res="NO HASHES (crashed?)"
  elif [ "$a" == "$b" ]; then res="SAME $(echo "$a" | wc -l) lines"
  else res="DIFFERS at $(diff <(echo "$a") <(echo "$b") | grep -m1 '^<' | cut -d' ' -f2)"; fi
  local ion ioff sk
  ion=$(grep -a "REC/icount" "$d/on/log.txt" | awk '{split($3,a,"=");s+=a[2];n++} END{print n?int(s/n):0}')
  ioff=$(grep -a "REC/icount" "$d/off/log.txt" | awk '{split($3,a,"=");s+=a[2];n++} END{print n?int(s/n):0}')
  sk=$(grep -a "REC/icount" "$d/on/log.txt" | sed 's/.*skipped=\([0-9]*\).*/\1/' | awk '{s+=$1;n++} END{print n?int(s/n):0}')
  printf '%-48.48s | %-22s | instr/frame %6s -> %6s | skipped %6s cycles/frame\n' "$n" "$res" "$ioff" "$ion" "$sk"
}
export -f one; export T HERE
ls "$ROMS"/*.gba | tr '\n' '\0' | xargs -0 -n1 -P "$JOBS" bash -c 'one "$0"' | sort
rm -rf "$T"
