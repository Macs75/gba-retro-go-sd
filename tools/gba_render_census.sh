#!/bin/bash
# gba_render_census.sh -- which text-layer renderer instances carry the
# drawing, over a set of games: pixels per frame per instance.
#
#   tools/gba_render_census.sh <census host binary> <rom dir> <frames> <out dir> [jobs]
#
# The binary: make host HOST_EXTRA_DEFS=-DGBA_RENDER_CENSUS (touch
# src/gpsp/video.cc first: objects are not rebuilt on a define change), copied
# somewhere. Per game, one host run from power-on with no input, unthrottled,
# windowless and silent; its RCENSUS lines go to <out dir>/<game>.txt.
# Power-on runs see intros, titles and attract demos, not gameplay: for the
# load of a game use its recording (GBA_REPLAY=1, see the loop at the end
# of this comment), not these.
#   cd <dir with g.gba + g.rec> && GBA_REPLAY=1 GBA_FAST=1 GBA_EXIT_FRAME=N \
#     SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy <binary> g.gba | grep RCENSUS
# Then: tools/gba_render_census.py <out dir>
set -u
BIN=$(readlink -f "${1:?census binary}"); ROMS=$(readlink -f "${2:?rom dir}")
FRAMES=${3:?frames}; OUT=$(readlink -f "${4:?out dir}"); JOBS=${5:-6}
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 GBA_EXIT_FRAME="$FRAMES" BIN OUT
one() {
  local rom="$1" n d
  n=$(basename "$rom" .gba | tr -c 'A-Za-z0-9\n' _)
  d=$(mktemp -d)
  ln -sf "$rom" "$d/g.gba"
  (cd "$d" && timeout "${GBA_CENSUS_TIMEOUT:-90}" "$BIN" g.gba 2>/dev/null | grep -a RCENSUS > "$OUT/$n.txt")
  rm -f "$d/g.gba"; rmdir "$d" 2>/dev/null
}
export -f one
find "$ROMS" -maxdepth 1 -name '*.gba' -print0 | xargs -0 -P "$JOBS" -I{} bash -c 'one "$@"' _ {}
echo "done: $(ls "$OUT" | wc -l) games in $OUT"
