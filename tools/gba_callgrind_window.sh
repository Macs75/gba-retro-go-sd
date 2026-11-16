#!/bin/bash
# gba_callgrind_window.sh -- callgrind over a window of a replay only.
#
#   tools/gba_callgrind_window.sh <host binary> <dir with g.gba + g.rec> <from> <to> <out.callgrind>
#
# Runs the replay under valgrind with instrumentation OFF (a few times slower
# than native, not ~50x), switches it on when the replay logs frame <from>
# (its REC/hash line: <from> must be a multiple of 120, GBA_HASH_EVERY),
# and exits at frame <to>. Then: callgrind_annotate <out.callgrind>.
# Windowless and silent; unthrottled (GBA_FAST); stdout line-buffered (stdbuf),
# else the start line reaches the log only at exit.
set -u
bin=$1; dir=$2; from=$3; to=$4; out=$5
log=$out.log
cd "$dir" || exit 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GBA_FAST=1 GBA_REPLAY=1 GBA_EXIT_FRAME=$to \
  stdbuf -oL valgrind --tool=callgrind --instr-atstart=no --callgrind-out-file="$out" "$bin" g.gba > "$log" 2>&1 &
pid=$!
# wait for the start frame's hash line (at most 10 minutes)
for _ in $(seq 1 1200); do
  grep -q "REC/hash frame=$from " "$log" && break
  kill -0 $pid 2>/dev/null || { echo "exited before frame $from" >&2; exit 1; }
  sleep 0.5
done
callgrind_control -i on $pid > /dev/null
wait $pid
echo "$out: frames $from-$to"
