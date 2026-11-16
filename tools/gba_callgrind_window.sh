#!/bin/bash
# gba_callgrind_window.sh -- callgrind over a window of a replay only.
#
#   tools/gba_callgrind_window.sh <host binary> <dir with g.gba + g.rec> <from> <to> <out.callgrind>
#
# Runs the replay under valgrind with instrumentation OFF (a few times slower
# than native, not ~50x); the host switches it on at emulated frame <from>
# (GBA_CALLGRIND_FROM, host_emu.c) and exits at frame <to> (GBA_EXIT_FRAME):
# exactly frames <from>..<to>-1, so two binaries' windows compare. (Switching
# it on from outside, on a log line, landed tens of frames late, a different
# amount each run.) Then: callgrind_annotate <out.callgrind>.
# Windowless and silent; unthrottled (GBA_FAST).
set -u
bin=$1; dir=$2; from=$3; to=$4; out=$5
cd "$dir" || exit 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 GBA_REPLAY=1 \
  GBA_CALLGRIND_FROM=$from GBA_EXIT_FRAME=$to \
  valgrind --tool=callgrind --instr-atstart=no --callgrind-out-file="$out" "$bin" g.gba > "$out.log" 2>&1
echo "$out: frames $from-$to"
