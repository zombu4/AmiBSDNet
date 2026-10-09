#!/usr/bin/env python3
"""Print the hunk layout of an AmigaOS executable and, optionally, check
whether given hunk-0 offsets carry a HUNK_RELOC32 entry.

Usage: python -I tools/hunkinfo.py <exe> [hex-offset ...]
"""
import struct
import sys

NAMES = {0x3E9: "CODE", 0x3EA: "DATA", 0x3EB: "BSS", 0x3EC: "RELOC32",
         0x3F0: "SYMBOL", 0x3F1: "DEBUG", 0x3F2: "END", 0x3F7: "DREL32",
         0x3FC: "RELOC32SHORT"}


def main():
    data = open(sys.argv[1], "rb").read()
    want = {int(a, 16) for a in sys.argv[2:]}
    pos = 0

    def u32():
        nonlocal pos
        v = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        return v

    assert u32() == 0x3F3, "not a hunk executable"
    while u32():            # resident library names (none expected)
        pass
    nhunks = u32()
    first, last = u32(), u32()
    sizes = [u32() for _ in range(last - first + 1)]
    for i, s in enumerate(sizes):
        mem = {0: "any", 1: "CHIP", 2: "FAST", 3: "ext"}[s >> 30]
        print(f"hunk {i}: {(s & 0x3FFFFFFF) * 4} bytes, memory={mem}")
    hunk = -1
    relocs = {}
    while pos < len(data):
        t = u32() & 0x3FFFFFFF
        if t in (0x3E9, 0x3EA):
            hunk += 1
            n = u32()
            pos += n * 4
        elif t == 0x3EB:
            hunk += 1
            u32()
        elif t == 0x3EC:
            while True:
                n = u32()
                if n == 0:
                    break
                target = u32()
                for _ in range(n):
                    relocs.setdefault(hunk, []).append((u32(), target))
        elif t == 0x3F0:
            while True:
                n = u32()
                if n == 0:
                    break
                pos += n * 4 + 4
        elif t == 0x3F1:
            pos += u32() * 4
        elif t == 0x3F2:
            pass
        else:
            print(f"unknown hunk type {t:#x} at {pos - 4:#x}")
            break
    for h, r in sorted(relocs.items()):
        print(f"hunk {h}: {len(r)} RELOC32 entries")
    if want:
        offs = {o: tgt for o, tgt in relocs.get(0, [])}
        for w in sorted(want):
            print(f"  offset {w:#x}: " +
                  (f"RELOC32 -> hunk {offs[w]}" if w in offs else "no reloc"))


if __name__ == "__main__":
    main()
