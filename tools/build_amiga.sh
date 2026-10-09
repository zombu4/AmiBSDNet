#!/bin/sh
# Build a plain Amiga program (no rump kernel), e.g. a bsdsocket client.
# usage: tools/build_amiga.sh <file.c> <output> [extra gcc flags]
set -e
python -I "$(dirname "$0")/gen/gen_inline.py" >/dev/null
cd "$(dirname "$0")/.."
T=toolchain/opt/bin
src=$1
out=$2
shift 2
name=$(basename "$src" .c)
$T/m68k-amiga-elf-gcc.exe -m68020 -O2 -ffreestanding -fno-builtin \
    -fno-tree-loop-distribute-patterns -Wall -Wno-unused-parameter \
    -Wno-volatile-register-var -Wno-pointer-sign -Wno-array-bounds \
    -ffile-prefix-map=$(pwd)=. -Isrc/include -Ibuild/gen "$@" \
    -nostdlib -Wl,--emit-relocs,--gc-sections,-Ttext=0,-e,_start \
    "$src" -o "build/$name.elf"
python -I tools/elf2hunk.py "build/$name.elf" "$out"
