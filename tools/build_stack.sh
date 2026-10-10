#!/bin/sh
# Build the AmiBSDNet stack program (kernel libraries must be built first
# with tools/build.py).
# usage: tools/build_stack.sh [output]          (default build/AmiBSDNet)
set -e
cd "$(dirname "$0")/.."
T=toolchain/opt/bin
OUT=${1:-build/AmiBSDNet}
HF="-m68020-60 -msoft-float -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
    -Wall -Wno-unused-parameter -Wno-volatile-register-var -Wno-pointer-sign -Wno-array-bounds \
    -ffile-prefix-map=$(pwd)=. -Inetbsd-src/sys/rump/include -Isrc/host \
    -Isrc/lib -Isrc/stack -Isrc/include -Ibuild/gen"
mkdir -p build/stackobj
python -I tools/gen/gen_bsdsocket.py
objs=""
for src in src/stack/main.c src/stack/*.c src/host/*.c src/lib/*.c src/common/*.c; do
	obj=build/stackobj/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')
	case " $objs " in *" $obj "*) continue ;; esac
	$T/m68k-amiga-elf-gcc.exe $HF -c "$src" -o "$obj"
	objs="$objs $obj"
done
$T/m68k-amiga-elf-gcc.exe -m68020-60 -c build/gen/bsdsocket_vec.s \
    -o build/stackobj/bsdsocket_vec.o
$T/m68k-amiga-elf-gcc.exe -m68020-60 -nostdlib \
    -Wl,--emit-relocs,--gc-sections,-Ttext=0,-e,_start,-Map=build/AmiBSDNet.map \
    $objs build/stackobj/bsdsocket_vec.o \
    -Wl,--whole-archive build/libamibsdnet.a build/librumpnet_net.a \
    build/librumpnet.a build/librumpkern.a -Wl,--no-whole-archive \
    -o build/AmiBSDNet.elf
python -I tools/elf2hunk.py build/AmiBSDNet.elf "$OUT"
