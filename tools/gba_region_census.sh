#!/bin/bash
# gba_region_census.sh -- where each game's executed instructions run from
# (BIOS / EWRAM / IWRAM / ROM), over a set of games, skipping the boot.
#
#   tools/gba_region_census.sh <rom dir> <from frame> <to frame> <out dir> [jobs]
#
# Uses the host's PC histogram (src/porting/gba_pchist.c, GBA_PCHIST=1): one
# run per game from power-on with no input, counting from <from frame> to
# <to frame> as one block. The PCHIST lines go to <out dir>/<game>.txt.
set -u
ROMS=$(readlink -f "${1:?rom dir}"); FROM=${2:?from}; TO=${3:?to}
OUT=$(readlink -f "${4:?out dir}"); JOBS=${5:-6}
HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 GBA_EXIT_FRAME="$TO" \
       GBA_PCHIST=1 GBA_PCHIST_FROM="$FROM" GBA_PCHIST_EVERY=$((TO - FROM)) GBA_PCHIST_TOP=0 HERE OUT
one() {
  local rom="$1" n d
  n=$(basename "$rom" .gba | tr -c 'A-Za-z0-9\n' _)
  d=$(mktemp -d)
  ln -sf "$rom" "$d/g.gba"
  (cd "$d" && timeout "${GBA_CENSUS_TIMEOUT:-120}" "$HERE/gba_host" g.gba 2>&1 | grep -a '^PCHIST frames' > "$OUT/$n.txt")
  rm -f "$d/g.gba"; rmdir "$d" 2>/dev/null
}
export -f one
find "$ROMS" -maxdepth 1 -name '*.gba' -print0 | xargs -0 -P "$JOBS" -I{} bash -c 'one "$@"' _ {}
echo "done: $(ls "$OUT" | wc -l) games in $OUT"
