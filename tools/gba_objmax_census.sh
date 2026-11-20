#!/bin/bash
# gba_objmax_census.sh -- the most sprites each game puts on one line at one
# priority (after the hardware's per-line OBJ cycle budget), and in the OBJ
# window: how deep a per-priority sprite list must be to drop nothing.
#
#   tools/gba_objmax_census.sh <rom dir> [frames] [jobs] [seed] > out.txt
#
# Per game: a host run from power-on with random buttons (GBA_MONKEY=<seed>,
# host_emu.c: through the menus into play, where a game gets that far) and
# GBA_OBJMAX=1 (video.cc), in a temporary directory: nothing is written next
# to the ROMs. Prints "<game>|<max on one priority>|<max OBJ window>|<frame
# of the max>", sorted by the first. GBA_CENSUS_TIMEOUT (default 150 s) per
# run. GBA_MONKEY_START=<pct>: Start after the first minute (host_emu.c).
# GBA_CENSUS_SHOTS=<dir>: each game's last frame there as <game>.png, to see
# which games reached play. Recordings: GBA_REPLAY=1 GBA_OBJMAX=1 on their own
# directories.
set -u
ROMS=$(readlink -f "${1:?rom dir}"); FRAMES=${2:-18000}; JOBS=${3:-6}; SEED=${4:-1}
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
SHOTS=${GBA_CENSUS_SHOTS:-}
[ -n "$SHOTS" ] && mkdir -p "$SHOTS" && SHOTS=$(readlink -f "$SHOTS")
export HERE T FRAMES SEED SHOTS
one() {
  local rom="$1" n d last
  n=$(basename "$rom" .gba); d="$T/$(echo "$n" | tr -c 'A-Za-z0-9\n' _)"
  mkdir -p "$d"; ln -sf "$rom" "$d/g.gba"
  local shot=
  [ -n "$SHOTS" ] && shot="$((FRAMES - 10)):$d/last.ppm"
  last=$(cd "$d" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 \
         GBA_MONKEY=$SEED GBA_OBJMAX=1 GBA_EXIT_FRAME=$FRAMES GBA_SHOT="$shot" \
         timeout "${GBA_CENSUS_TIMEOUT:-150}" "$HERE/gba_host" g.gba 2>/dev/null |
         grep -a "^OBJMAX " | tail -1)
  [ -n "$SHOTS" ] && [ -f "$d/last.ppm" ] &&
    python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" \
      "$d/last.ppm" "$SHOTS/$n.png" 2>/dev/null
  local p w f
  p=$(echo "$last" | sed -n 's/.*prio=\([0-9]*\).*/\1/p')
  w=$(echo "$last" | sed -n 's/.*win=\([0-9]*\).*/\1/p')
  f=$(echo "$last" | sed -n 's/.*frame=\([0-9]*\).*/\1/p')
  echo "$n|${p:-0}|${w:-0}|${f:--}"
}
export -f one
find "$ROMS" -maxdepth 1 -name '*.gba' -print0 | xargs -0 -P "$JOBS" -I{} bash -c 'one "$1"' _ {} |
  sort -t'|' -k2 -n -r
rm -rf "$T"
