#!/bin/bash
# gba_fn_codegen.sh -- compile ONE device object in the release Docker
# toolchain and print a function's size, instruction count, stack accesses
# and literal-pool loads: a quick register-pressure check of a source change
# (catalogue §40: judge an interpreter/renderer edit by the WHOLE function's
# spills, not by the edited lines).
#
#   tools/gba_fn_codegen.sh <object, e.g. build/cpu.o> <section, e.g. .text.execute_arm> [make vars...]
#
# Uses the same Docker image as `make docker`; builds only that object
# (PROF=1 PCS=0 unless overridden), so a variant costs seconds, not a link.
set -u
obj=$1; sec=$2; shift 2
cd "$(dirname "$0")/.." || exit 1
img=$(make -s -f Makefile -f - print-img <<'EOF' 2>/dev/null
print-img: ; @echo $(DOCKER_IMAGE)
EOF
)
[ -z "$img" ] && img=sylverb/retro-go-sd-builder:v1.5
rm -f "$obj"
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/opt/workdir" -w /opt/workdir "$img" \
  make --no-print-directory PROF=1 PCS=0 "$@" "$obj" >/dev/null 2>"$obj.log" || { tail -20 "$obj.log"; exit 1; }
d=$(arm-none-eabi-objdump -d "$obj" --section="$sec")
size=$(arm-none-eabi-size -A "$obj" | awk -v s="$sec" '$1==s{print $2}')
printf "%s %s: %d bytes, %d insns, %d [sp] accesses, %d literal loads\n" "$obj" "$sec" "$size" \
  "$(echo "$d" | grep -cE '^\s+[0-9a-f]+:\s')" "$(echo "$d" | grep -c '\[sp')" \
  "$(echo "$d" | grep -cE 'ldr(\.w)?\s+r[0-9]+, \[pc')"
