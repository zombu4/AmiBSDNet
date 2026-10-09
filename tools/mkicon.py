#!/usr/bin/env python3
"""Write classic Workbench .info icon files (DiskObject, 2 bitplanes,
Workbench default palette: 0 grey, 1 black, 2 white, 3 blue).

Icons are drawn from small text "pixel art" so no binary images live in
the repository.

Usage (as a module):  write_icon(path, kind, art, default_tool=None,
                                 tooltypes=(), stack=0)
  kind: "tool", "project" or "drawer"
"""
import struct

WB_DISKMAGIC = 0xE310
WB_DISKVERSION = 1
WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4
NO_ICON_POSITION = 0x80000000
GFLG_GADGIMAGE = 0x0004
GFLG_GADGHCOMP = 0x0000
GFLG_GADGHIMAGE = 0x0002
GACT_RELVERIFY = 0x0001
GACT_IMMEDIATE = 0x0002
GTYP_BOOLGADGET = 0x0001

PENS = {".": 0, "#": 1, "o": 2, "*": 3}


def planar(art):
    h = len(art)
    w = max(len(r) for r in art)
    wpr = (w + 15) // 16
    planes = []
    for plane in range(2):
        data = bytearray()
        for row in art:
            words = [0] * wpr
            for x, ch in enumerate(row):
                if PENS.get(ch, 0) & (1 << plane):
                    words[x // 16] |= 0x8000 >> (x % 16)
            data += struct.pack(f">{wpr}H", *words)
        planes.append(data)
    return w, h, planes[0] + planes[1]


def image(art, offset=0):
    w, h, data = planar(art)
    # struct Image: Left, Top, Width, Height, Depth, ImageData*,
    #               PlanePick, PlaneOnOff, NextImage*
    return struct.pack(">hhhhhIBBI", 0, 0, w, h, 2, 1, 3, 0, 0) + data, w, h


def invert(art):
    """highlight image: swap grey/blue and black/white"""
    swap = {".": "*", "*": ".", "#": "o", "o": "#"}
    return [("".join(swap.get(c, c) for c in row)) for row in art]


def string(s):
    b = s.encode("latin-1") + b"\0"
    return struct.pack(">I", len(b)) + b


def write_icon(path, kind, art, default_tool=None, tooltypes=(), stack=0):
    dtype = {"tool": WBTOOL, "project": WBPROJECT, "drawer": WBDRAWER}[kind]
    img1, w, h = image(art)
    img2, _, _ = image(invert(art))
    drawer = dtype == WBDRAWER

    gadget = struct.pack(
        ">IhhhhHHHIIIIIHI",
        0, 0, 0, w, h + 1,
        GFLG_GADGIMAGE | GFLG_GADGHIMAGE,
        GACT_RELVERIFY | GACT_IMMEDIATE,
        GTYP_BOOLGADGET,
        1, 1, 0, 0, 0, 0,
        1)                      # UserData: revision 1 (OS 2.x+)
    dobj = struct.pack(">HH", WB_DISKMAGIC, WB_DISKVERSION) + gadget
    dobj += struct.pack(">BBIIiiIIi",
                        dtype, 0,
                        1 if default_tool else 0,
                        1 if tooltypes else 0,
                        NO_ICON_POSITION - (1 << 32), NO_ICON_POSITION - (1 << 32),
                        1 if drawer else 0,
                        0,
                        stack)
    assert len(dobj) == 78, len(dobj)
    out = bytearray(dobj)
    if drawer:
        # OldDrawerData: struct NewWindow (48) + CurrentX + CurrentY
        nw = struct.pack(">hhhhBBIIIIIIIhhHHH",
                         50, 50, 400, 150, 255, 255, 0, 0x0240027f,
                         0, 0, 0, 0, 0, 90, 40, 0xffff, 0xffff, 1)
        out += nw + struct.pack(">ii", 0, 0)
    out += img1 + img2
    if default_tool:
        out += string(default_tool)
    if tooltypes:
        out += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            out += string(t)
    if drawer:
        # DrawerData2 (OS 2.x): dd_Flags, dd_ViewModes
        out += struct.pack(">IH", 0, 0)
    open(path, "wb").write(out)


# ---------------------------------------------------------------------------
# AmiBSDNet artwork (32 x 22)

ART_STACK = [
    "................................",
    "..##################............",
    "..#oooooooooooooooo#............",
    "..#o**************o#............",
    "..#o*oooooo*******o#....#####...",
    "..#o**************o#....#***#...",
    "..#o*oooooooooo***o#....#***#...",
    "..#o**************o#*****###*...",
    "..#o*oooo*********o#....#***#...",
    "..#o**************o#....#***#...",
    "..#o**************o#....#####...",
    "..#oooooooooooooooo#......*.....",
    "..##################......*.....",
    "........######............*.....",
    "........######............*.....",
    ".....############.........*.....",
    ".....############.........*.....",
    "..........................*.....",
    "....*******************************",
    "................................",
    "................................",
    "................................",
]

ART_STATUS = [
    "................................",
    "......##############............",
    ".....#oooooooooooooo#...........",
    "....#oo************oo#..........",
    "....#o**oooooooooo**o#..........",
    "....#o*oo********oo*o#..........",
    "....#o*o**oooooo**o*o#..........",
    "....#o*o*oo****oo*o*o#..........",
    "....#o*o*o**oo**o*o*o#..........",
    "....#o*o*o*o##o*o*o*o#..........",
    "....#o*o*o*o##o*o*o*o#..........",
    "....#o**************o#..........",
    ".....#oooooooooooooo#...........",
    "......######**######............",
    "...........#**#.................",
    "...........#**#.................",
    ".......############.............",
    ".......#oooooooooo#.............",
    ".......############.............",
    "................................",
    "................................",
    "................................",
]

ART_INSTALL = [
    "................................",
    "....####################........",
    "....#oooooooooooooooooo#........",
    "....#o################o#........",
    "....#o#**************#o#........",
    "....#o#*####*****###*#o#........",
    "....#o#**************#o#........",
    "....#o#*##########***#o#........",
    "....#o#**************#o#........",
    "....#o################o#........",
    "....#oooooooooooooooooo#........",
    "....####################........",
    "..........#######...............",
    "..........#*****#...............",
    "........###*****###.............",
    ".........#*******#..............",
    "..........#*****#...............",
    "...........#***#................",
    "............#*#.................",
    ".............#..................",
    "................................",
    "................................",
]

ART_DRAWER = [
    "................................",
    "..############################..",
    "..#oooooooooooooooooooooooooo#..",
    "..#o########################o#..",
    "..#o#**********************#o#..",
    "..#o#*******########*******#o#..",
    "..#o#**********************#o#..",
    "..#o########################o#..",
    "..#oooooooooooooooooooooooooo#..",
    "..#o########################o#..",
    "..#o#**********************#o#..",
    "..#o#*******########*******#o#..",
    "..#o#**********************#o#..",
    "..#o########################o#..",
    "..#oooooooooooooooooooooooooo#..",
    "..############################..",
    "................................",
    "................................",
]

ART_DOC = [
    "................................",
    "......################..........",
    "......#oooooooooooooo##.........",
    "......#oooooooooooooo#o#........",
    "......#o##########ooo####.......",
    "......#ooooooooooooooooo#.......",
    "......#o#############ooo#.......",
    "......#ooooooooooooooooo#.......",
    "......#o###########ooooo#.......",
    "......#ooooooooooooooooo#.......",
    "......#o##############oo#.......",
    "......#ooooooooooooooooo#.......",
    "......#o#########oooooo*#.......",
    "......#ooooooooooooooo***.......",
    "......###################.......",
    "................................",
]
