#!/bin/sh
# Build the host side and link a test program against the rump libraries.
# usage: tools/link_test.sh <src/test/name.c> <output>
set -e
cd "$(dirname "$0")/.."
T=toolchain/opt/bin
HF="-m68020-60 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
    -Wall -Wno-unused-parameter -Wno-volatile-register-var -Wno-pointer-sign -Wno-array-bounds \
    -ffile-prefix-map=$(pwd)=. -Inetbsd-src/sys/rump/include -Isrc/host"
name=$(basename "$1" .c)
hostobjs=""
for src in src/host/*.c; do
	obj=build/host_$(basename "$src" .c).o
	$T/m68k-amiga-elf-gcc.exe $HF -c "$src" -o "$obj"
	hostobjs="$hostobjs $obj"
done
$T/m68k-amiga-elf-gcc.exe $HF -c "$1" -o "build/$name.o"
$T/m68k-amiga-elf-gcc.exe -m68020-60 -nostdlib \
    -Wl,--emit-relocs,--gc-sections,-Ttext=0,-e,_start,-Map=build/$name.map \
    "build/$name.o" \
    -Wl,--whole-archive build/libamibsdnet.a build/librumpnet_net.a \
    build/librumpnet.a build/librumpkern.a -Wl,--no-whole-archive \
    $hostobjs -o "build/$name.elf"
python -I tools/elf2hunk.py "build/$name.elf" "$2"
