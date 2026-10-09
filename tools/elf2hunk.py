#!/usr/bin/env python3
"""Convert a statically linked m68k ELF (linked at address 0 with
--emit-relocs) into an AmigaOS hunk executable.

Why not the bundled elf2hunk: it emits HUNK_RELOC32 entries for
R_68K_PC32 relocations (e.g. cross-object 'bra.l' tail calls), which makes
LoadSeg add the load address to a PC-relative displacement.

Layout: every allocated section (.text, .rodata, link sets, init/fini
arrays, .data, .bss) goes into ONE code hunk at exactly its ELF address,
so PC-relative references between them are already correct and only
absolute R_68K_32 relocations need HUNK_RELOC32 entries.  .bss is stored
as explicit zeros.  The hunk is flagged MEMF_FAST (header size word and
hunk type), so LoadSeg will only place it in Fast RAM.

Usage: python -I tools/elf2hunk.py in.elf out-exe [--any-mem]
"""
import struct
import sys

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_RELA = 1, 2, 8, 4
SHT_INIT_ARRAY, SHT_FINI_ARRAY, SHT_PREINIT_ARRAY = 14, 15, 16
SHF_ALLOC = 0x2
SHN_UNDEF, SHN_ABS = 0, 0xFFF1

R_68K_NONE, R_68K_32, R_68K_16, R_68K_8 = 0, 1, 2, 3
R_68K_PC32, R_68K_PC16, R_68K_PC8 = 4, 5, 6

HUNK_HEADER, HUNK_CODE, HUNK_RELOC32, HUNK_END = 0x3F3, 0x3E9, 0x3EC, 0x3F2
HUNKF_FAST = 1 << 31


def die(msg):
    sys.exit(f"elf2hunk.py: {msg}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    anymem = "--any-mem" in sys.argv
    if len(args) != 2:
        die("usage: elf2hunk.py in.elf out-exe [--any-mem]")
    elf = open(args[0], "rb").read()

    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 2:
        die("not a 32-bit big-endian ELF")
    (e_type, e_machine, _, _, _, e_shoff, _, _, _, _,
     e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(
        ">HHIIIIIHHHHHH", elf, 16)
    if e_machine != 4:
        die("not an m68k ELF")

    secs = []
    for i in range(e_shnum):
        (name, stype, flags, addr, off, size, link, info, align,
         entsize) = struct.unpack_from(">IIIIIIIIII", elf,
                                       e_shoff + i * e_shentsize)
        secs.append(dict(name=name, type=stype, flags=flags, addr=addr,
                         off=off, size=size, link=link, info=info))
    strtab = secs[e_shstrndx]

    def secname(s):
        o = strtab["off"] + s["name"]
        return elf[o:elf.index(b"\0", o)].decode()

    alloc = [s for s in secs if s["flags"] & SHF_ALLOC and s["size"]]
    if not alloc:
        die("no allocated sections")
    if min(s["addr"] for s in alloc) != 0:
        die("image must be linked at address 0 (-Ttext=0)")
    end = max(s["addr"] + s["size"] for s in alloc)
    end = (end + 3) & ~3
    image = bytearray(end)
    for s in alloc:
        if s["type"] != SHT_NOBITS:
            image[s["addr"]:s["addr"] + s["size"]] = \
                elf[s["off"]:s["off"] + s["size"]]

    # symbol section indices, to skip absolute / undefined-weak targets
    def sym_shndx(symtab, idx):
        o = symtab["off"] + idx * 16
        return struct.unpack_from(">IIIBBH", elf, o)[5]

    relocs = []
    counts = {}
    for r in secs:
        if r["type"] != SHT_RELA:
            continue
        target = secs[r["info"]]
        if not (target["flags"] & SHF_ALLOC):
            continue        # relocations for debug sections etc.
        symtab = secs[r["link"]]
        for k in range(r["size"] // 12):
            r_off, r_info, r_add = struct.unpack_from(
                ">IIi", elf, r["off"] + k * 12)
            rtype, rsym = r_info & 0xFF, r_info >> 8
            counts[rtype] = counts.get(rtype, 0) + 1
            # ET_EXEC (--emit-relocs): r_offset is already a virtual address;
            # ET_REL: it is relative to the target section
            where = r_off if e_type == 2 else target["addr"] + r_off
            if not (target["addr"] <= where < target["addr"] + target["size"]):
                die(f"relocation offset {where:#x} outside {secname(target)}")
            if rtype in (R_68K_NONE, R_68K_PC32, R_68K_PC16, R_68K_PC8):
                continue    # already resolved within the single hunk
            if rtype != R_68K_32:
                die(f"unsupported relocation type {rtype} at {where:#x} "
                    f"in {secname(target)}")
            if rsym and sym_shndx(symtab, rsym) in (SHN_ABS, SHN_UNDEF):
                continue    # absolute value or undefined weak (0)
            if where & 1:
                die(f"odd RELOC32 offset {where:#x}")
            relocs.append(where)
    relocs.sort()

    nwords = end // 4
    memflag = 0 if anymem else HUNKF_FAST
    out = bytearray()
    out += struct.pack(">IIIII", HUNK_HEADER, 0, 1, 0, 0)
    out += struct.pack(">I", nwords | memflag)
    out += struct.pack(">II", HUNK_CODE | memflag, nwords)
    out += image
    if relocs:
        out += struct.pack(">I", HUNK_RELOC32)
        for i in range(0, len(relocs), 65535):
            chunk = relocs[i:i + 65535]
            out += struct.pack(">II", len(chunk), 0)
            out += struct.pack(f">{len(chunk)}I", *chunk)
        out += struct.pack(">I", 0)
    out += struct.pack(">I", HUNK_END)
    open(args[1], "wb").write(out)

    names = {R_68K_32: "32", R_68K_PC32: "PC32", R_68K_PC16: "PC16",
             R_68K_PC8: "PC8", R_68K_NONE: "NONE"}
    summary = ", ".join(f"{names.get(t, t)}={n}" for t, n in sorted(counts.items()))
    print(f"elf2hunk.py: 1 hunk, {end} bytes ({'any' if anymem else 'FAST'} "
          f"memory), {len(relocs)} RELOC32 [{summary}]")


if __name__ == "__main__":
    main()
