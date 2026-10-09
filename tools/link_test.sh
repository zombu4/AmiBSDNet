#!/bin/sh
# Build the host side and link a test program against the rump libraries.
# usage: tools/link_test.sh <src/test/name.c> <output>
set -e
cd "$(dirname "$0")/.."
T=toolchain/opt/bin
HF="-m68040 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
    -Wall -Wno-unused-parameter -Wno-volatile-register-var -Wno-pointer-sign \
    -ffile-prefix-map=$(pwd)=. -Inetbsd-src/sys/rump/include -Isrc/host"
name=$(basename "$1" .c)
$T/m68k-amiga-elf-gcc.exe $HF -c src/host/rumpuser_amiga.c -o build/rumpuser_amiga.o
$T/m68k-amiga-elf-gcc.exe $HF -c src/host/crashtrap.c -o build/crashtrap.o
$T/m68k-amiga-elf-gcc.exe $HF -c "$1" -o "build/$name.o"
$T/m68k-amiga-elf-gcc.exe -m68040 -nostdlib \
    -Wl,--emit-relocs,--gc-sections,-Ttext=0,-e,_start,-Map=build/$name.map \
    "build/$name.o" \
    -Wl,--whole-archive build/librumpnet_net.a build/librumpnet.a build/librumpkern.a \
    -Wl,--no-whole-archive build/rumpuser_amiga.o build/crashtrap.o -o "build/$name.elf"
python -I tools/elf2hunk.py "build/$name.elf" "$2"
