#!/usr/bin/env python3
"""Fetch the third-party pieces this repository builds against, at pinned
versions, into git-ignored directories:

  toolchain/   Bartman's m68k-amiga-elf GCC + binutils (native Windows),
               extracted from the vscode-amiga-debug release package
  netbsd-src/  sparse checkout of the NetBSD source tree (kernel parts)
  emu/winuae/  WinUAE (portable), for running tests
  downloads/WirelessManager
               the 68k WirelessManager binary shipped in the package
  downloads/Emu68-WiFi/
               wifipi.device and Wi-Fi firmware (Networks/, Firmware/)
               shipped in the package

Downloads are verified against SHA-256 hashes.  Re-running is safe.

Usage: python -I tools/bootstrap.py [--no-emu]
"""
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DL = os.path.join(TOP, "downloads")

TOOLCHAIN = dict(
    url="https://github.com/BartmanAbyss/vscode-amiga-debug/releases/"
        "download/1.8.2/amiga-debug-1.8.2.vsix",
    sha256="c1e2b11175a5c5036d47eef095a21a616fbeb9f1a681364519c7910583d7ee3e",
    prefix="extension/bin/win32/",
    dest="toolchain")

WINUAE = dict(
    url="https://download.abime.net/winuae/releases/WinUAE6030_x64.zip",
    sha256="f8b7c44e8ab2f68db49b4205793848b4496464dbee3c036921840d82627f4fc3",
    prefix="",
    dest=os.path.join("emu", "winuae"))

# WirelessManager (wpa_supplicant for AmigaOS, Neil Cafferkey, BSD licence)
# for WPA Wi-Fi; shipped in the AmiBSDNet package.  From the prism2v2
# driver archive on Aminet; LHA is unpacked with 7-Zip.
PRISM2V2 = dict(
    url="https://aminet.net/driver/net/prism2v2.lha",
    sha256="25b400ef25c44af940e1887576106b8312dcd28fe6c8030fcb78804480c07bc5",
    member="prism2v2/C/WirelessManager",
    dest=os.path.join("downloads", "WirelessManager"))

# Emu68 Wi-Fi: wifipi.device (Michal Schulz, MPL-2.0) and the Raspberry Pi
# Wi-Fi firmware, shipped in the package (see dist/Docs/ThirdParty.txt)
EMU68TOOLS = dict(
    url="https://github.com/michalsc/Emu68-tools/releases/download/v1.1/"
        "Emu68-tools.zip",
    sha256="d8386650d9f6094a0b858fc62619c58188acf2037b98a55452a5331d29f6ec1a",
    prefix="Emu68-WiFi/Devs/",
    dest=os.path.join("downloads", "Emu68-WiFi"))

NETBSD_REPO = "https://github.com/NetBSD/src"
NETBSD_BRANCH = "netbsd-11"
NETBSD_COMMIT = "0d0a71cca9c550e6392fc8bc50726e920b55a778"
NETBSD_PATHS = """
    sys/rump sys/kern sys/net sys/netinet sys/netinet6 sys/sys
    sys/lib/libkern sys/uvm sys/crypto sys/secmodel sys/compat/common
    sys/compat/net sys/compat/netinet sys/compat/netinet6 sys/compat/sys
    sys/arch/m68k/include sys/arch/amiga/include sys/conf sys/dev
    sys/netatalk sys/netipsec sys/altq sys/ufs sys/net80211 sys/netmpls
    common/lib/libc common/lib/libutil common/lib/libprop common/lib/libppath
    common/include share/mk include lib/librumpuser lib/librumpclient
""".split()


def fetch(spec):
    os.makedirs(DL, exist_ok=True)
    path = os.path.join(DL, os.path.basename(spec["url"]))
    if not os.path.exists(path):
        print(f"downloading {spec['url']}")
        urllib.request.urlretrieve(spec["url"], path + ".part")
        os.replace(path + ".part", path)
    h = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if h != spec["sha256"]:
        sys.exit(f"bootstrap: SHA-256 mismatch for {path}\n"
                 f"  expected {spec['sha256']}\n  got      {h}")
    return path


def extract(spec):
    dest = os.path.join(TOP, spec["dest"])
    if os.path.isdir(dest) and os.listdir(dest):
        print(f"{spec['dest']}: present")
        return
    archive = fetch(spec)
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename
            if not name.startswith(spec["prefix"]) or info.is_dir():
                continue
            out = os.path.join(dest, name[len(spec["prefix"]):])
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(info) as src, open(out, "wb") as dst:
                dst.write(src.read())
    print(f"{spec['dest']}: extracted")


def seven_zip():
    for p in (shutil.which("7z"), r"C:\Program Files\7-Zip\7z.exe"):
        if p and os.path.exists(p):
            return p
    sys.exit("bootstrap: 7-Zip (7z) is needed to unpack LHA archives")


def wirelessmanager():
    out = os.path.join(TOP, PRISM2V2["dest"])
    if os.path.exists(out):
        print(f"{PRISM2V2['dest']}: present")
        return
    archive = fetch(PRISM2V2)
    tmp = os.path.join(DL, "prism2v2-unpacked")
    subprocess.run([seven_zip(), "x", "-y", f"-o{tmp}", archive,
                    PRISM2V2["member"].replace("/", os.sep)],
                   check=True, stdout=subprocess.DEVNULL)
    os.replace(os.path.join(tmp, *PRISM2V2["member"].split("/")), out)
    shutil.rmtree(tmp)
    print(f"{PRISM2V2['dest']}: extracted")


def git(*args, cwd=None):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def netbsd():
    dest = os.path.join(TOP, "netbsd-src")
    if not os.path.isdir(os.path.join(dest, ".git")):
        git("-c", "core.longpaths=true", "clone", "--depth", "1",
            "--filter=blob:none",
            "--no-checkout", "--branch", NETBSD_BRANCH, NETBSD_REPO, dest)
    # the full tree contains Windows-reserved names such as 'aux'; they are
    # outside the sparse set, but git must be allowed to index them
    git("config", "core.protectNTFS", "false", cwd=dest)
    git("config", "core.longpaths", "true", cwd=dest)
    git("sparse-checkout", "init", "--cone", cwd=dest)
    git("sparse-checkout", "set", *NETBSD_PATHS, cwd=dest)
    git("fetch", "--depth", "1", "origin", NETBSD_COMMIT, cwd=dest)
    git("checkout", "--detach", NETBSD_COMMIT, cwd=dest)
    print(f"netbsd-src: at {NETBSD_COMMIT[:12]}")


def main():
    extract(TOOLCHAIN)
    netbsd()
    wirelessmanager()
    extract(EMU68TOOLS)
    if "--no-emu" not in sys.argv:
        extract(WINUAE)


if __name__ == "__main__":
    main()
