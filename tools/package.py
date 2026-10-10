#!/usr/bin/env python3
"""Build the AmiBSDNet distribution: staged drawer, icons, LHA and ZIP.

  python -I tools/package.py [--no-build]

Without --no-build the kernel libraries, the stack and the tools are
built first (release flags: optimised, no kernel DIAGNOSTIC checks).
Output: build/dist/AmiBSDNet/, build/AmiBSDNet.lha, build/AmiBSDNet.zip
"""
import os
import shutil
import struct
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import mkicon  # noqa: E402

BUILD = os.path.join(TOP, "build")
STAGE = os.path.join(BUILD, "dist")
VERSION = "0.5"

BASH = shutil.which("bash") or r"C:\Program Files\Git\bin\bash.exe"


def run(cmd, env=None):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=TOP, check=True, env=env)


def build():
    env = dict(os.environ, RUMP_OPT="-O2")
    for d in ("obj", "stackobj"):
        shutil.rmtree(os.path.join(BUILD, d), ignore_errors=True)
    run([sys.executable, "-I", "tools/build.py"], env)
    run([BASH, "tools/build_stack.sh", "build/AmiBSDNet"])
    for src, out, extra in (
            ("src/tools/netctrl.c", "build/NetCtrl",
             ["src/common/probe.c", "src/tools/otherstacks.c",
              "src/common/drvcheck.c"]),
            ("src/tools/ping.c", "build/Ping", []),
            ("src/tools/status.c", "build/AmiBSDNetStatus",
             ["src/tools/wifiwin.c", "src/tools/settingswin.c",
              "src/common/wm.c", "src/common/probe.c",
              "src/common/drvcheck.c"])):
        run([BASH, "tools/build_amiga.sh", src, out] + extra)


# ---------------------------------------------------------------------------
# LHA (level 2 headers, -lh0- stored)

def crc16(data, crc=0):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def lha_entry(path_in_archive, data, mtime):
    parts = path_in_archive.split("/")
    name = parts[-1].encode("latin-1")
    dirs = parts[:-1]
    ext = b""
    ext += struct.pack("<H", 3 + len(name)) + b"\x01" + name
    if dirs:
        d = b"".join(p.encode("latin-1") + b"\xff" for p in dirs)
        ext += struct.pack("<H", 3 + len(d)) + b"\x02" + d
    ext += struct.pack("<H", 0)
    # base header (26 bytes incl. the first next-size field) + extensions;
    # each extension's size field precedes it, so the first one lives at
    # offset 24 of the base header.
    first_next = ext[:2]
    rest = ext[2:]
    hdr = bytearray()
    hdr += b"\0\0"                          # total header size (later)
    hdr += b"-lh0-"
    hdr += struct.pack("<II", len(data), len(data))
    hdr += struct.pack("<I", int(mtime))
    hdr += b"\x20"                          # reserved
    hdr += b"\x02"                          # level 2
    hdr += struct.pack("<H", crc16(data))
    hdr += b"A"                             # OS: Amiga
    hdr += first_next
    hdr += rest
    struct.pack_into("<H", hdr, 0, len(hdr))
    return bytes(hdr) + data


def write_lha(path, files):
    with open(path, "wb") as f:
        for arcname, src in files:
            data = open(src, "rb").read()
            f.write(lha_entry(arcname, data, os.path.getmtime(src)))
        f.write(b"\0")


# ---------------------------------------------------------------------------

def stage():
    shutil.rmtree(STAGE, ignore_errors=True)
    root = os.path.join(STAGE, "AmiBSDNet")
    for d in ("C", "Status", "Docs"):
        os.makedirs(os.path.join(root, d))

    for name in ("AmiBSDNet", "NetCtrl", "Ping"):
        shutil.copy(os.path.join(BUILD, name), os.path.join(root, "C", name))

    # third-party pieces for Wi-Fi (fetched by tools/bootstrap.py; their
    # licences are in Docs/ThirdParty.txt)
    dl = os.path.join(TOP, "downloads")
    wm = os.path.join(dl, "WirelessManager")
    wifi = os.path.join(dl, "Emu68-WiFi")
    if not os.path.exists(wm) or not os.path.isdir(wifi):
        sys.exit("package: WirelessManager / Emu68-WiFi missing; run "
                 "python -I tools/bootstrap.py")
    shutil.copy(wm, os.path.join(root, "C", "WirelessManager"))
    shutil.copytree(os.path.join(wifi, "Networks"),
                    os.path.join(root, "Devs", "Networks"))
    shutil.copytree(os.path.join(wifi, "Firmware"),
                    os.path.join(root, "Devs", "Firmware"))
    shutil.copy(os.path.join(TOP, "dist", "Docs", "ThirdParty.txt"),
                os.path.join(root, "Docs", "ThirdParty.txt"))
    shutil.copy(os.path.join(BUILD, "AmiBSDNetStatus"),
                os.path.join(root, "Status", "AmiBSDNetStatus"))
    shutil.copy(os.path.join(TOP, "dist", "Install"),
                os.path.join(root, "Install"))
    shutil.copy(os.path.join(TOP, "dist", "Docs", "AmiBSDNet.txt"),
                os.path.join(root, "Docs", "AmiBSDNet.txt"))
    shutil.copy(os.path.join(TOP, "LICENSE"),
                os.path.join(root, "Docs", "LICENSE.txt"))
    shutil.copy(os.path.join(TOP, "dist", "AmiBSDNet.readme"),
                os.path.join(STAGE, "AmiBSDNet.readme"))

    mkicon.write_icon(os.path.join(STAGE, "AmiBSDNet.info"), "drawer",
                      mkicon.ART_DRAWER)
    mkicon.write_icon(os.path.join(root, "Docs.info"), "drawer",
                      mkicon.ART_DRAWER)
    mkicon.write_icon(os.path.join(root, "Status.info"), "drawer",
                      mkicon.ART_DRAWER)
    mkicon.write_icon(os.path.join(root, "Install.info"), "project",
                      mkicon.ART_INSTALL, default_tool="Installer",
                      tooltypes=("APPNAME=AmiBSDNet", "MINUSER=AVERAGE",
                                 "DEFUSER=AVERAGE"))
    mkicon.write_icon(os.path.join(root, "Status", "AmiBSDNetStatus.info"),
                      "tool", mkicon.ART_STATUS, stack=8192,
                      tooltypes=("DONOTWAIT", "STACK=C:AmiBSDNet",
                                 "INTERVAL=2"))
    for doc in ("AmiBSDNet.txt", "LICENSE.txt", "ThirdParty.txt"):
        mkicon.write_icon(os.path.join(root, "Docs", doc + ".info"),
                          "project", mkicon.ART_DOC,
                          default_tool="SYS:Utilities/MultiView")
    return root


def main():
    if "--no-build" not in sys.argv:
        build()
    stage()
    files = []
    for dirpath, dirnames, filenames in os.walk(STAGE):
        dirnames.sort()
        for fn in sorted(filenames):
            src = os.path.join(dirpath, fn)
            files.append((os.path.relpath(src, STAGE).replace(os.sep, "/"),
                          src))
    lha = os.path.join(BUILD, "AmiBSDNet.lha")
    write_lha(lha, files)
    zp = os.path.join(BUILD, "AmiBSDNet.zip")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, src in files:
            z.write(src, arc)
    sizes = {n: os.path.getsize(os.path.join(BUILD, n))
             for n in ("AmiBSDNet", "NetCtrl", "Ping", "AmiBSDNetStatus")}
    print(f"package: {len(files)} files -> {lha} "
          f"({os.path.getsize(lha)} bytes), {zp}")
    for n, sz in sizes.items():
        print(f"  {n}: {sz} bytes")


if __name__ == "__main__":
    main()
