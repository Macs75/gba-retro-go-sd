#!/bin/bash
# gba_idle_census.sh -- where each game's ARM7 spends its instructions, and
# how much of it is a pure poll loop (tools/gba_pchist.py --classify).
#
#   tools/gba_idle_census.sh <rom dir> <frames> [jobs]
#
# Runs the host build unthrottled from power-on (no input: intro, title,
# attract) with GBA_PCHIST=1, one window = the last third of the run, JOBS
# games at a time (default 6), each stopped after GBA_CENSUS_TIMEOUT
# seconds (default 60). One line per game, sorted by the poll share.
# Attract mode is not gameplay: a loop found here must be confirmed in a
# recording of play before it is worth a skip.
set -u
ROMS=$(readlink -f "${1:?rom dir}"); FRAMES=${2:?frames}; JOBS=${3:-6}
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 GBA_PCHIST=1 GBA_EXIT_FRAME="$FRAMES"
export GBA_PCHIST_FROM=$((FRAMES * 2 / 3)) GBA_PCHIST_EVERY=$((FRAMES - FRAMES * 2 / 3 - 1)) GBA_PCHIST_TOP=40
run() {
  local rom="$1" d="$T/$(basename "$1" .gba | tr -c 'A-Za-z0-9\n' _)"
  mkdir -p "$d"
  (cd "$d" && timeout "${GBA_CENSUS_TIMEOUT:-60}" "$HERE/gba_host" "$rom" > log.txt 2>&1)
  python3 -I "$HERE/tools/gba_pchist.py" "$rom" "$d/log.txt" --classify "$(basename "$rom" .gba)"
}
export -f run; export T HERE
ls "$ROMS"/*.gba | tr '\n' '\0' | xargs -0 -n1 -P "$JOBS" bash -c 'run "$0"' | sort -t'|' -k2 -rn
rm -rf "$T"
