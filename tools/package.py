#!/usr/bin/env python3
"""Build the AmiBSDNet distribution: staged drawer, icons, LHA and ZIP.

  python -I tools/package.py [--no-build]

Without --no-build the kernel libraries, the stack and the tools are
built first (release flags: optimised, no kernel DIAGNOSTIC checks), the
NetBSD licence notices of what was compiled are collected into
dist/Docs/NetBSD.txt (tools/notices.py), and build/release.stamp records
the SHA-256 of every binary and of NetBSD.txt.  --no-build packages the
binaries of that release build again; it refuses if there is no stamp or
any of them has changed since (built again some other way).

The version comes from tools/version.py: it is put into the staged
AmiBSDNet.txt and the readme, and the staged Install script gets
lines in front that set #ver (the version) and, for every staged file,
#sum-<file> to "<size> <crc32>".

Output: build/dist/AmiBSDNet/, build/AmiBSDNet.lha, build/AmiBSDNet.zip
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import mkicon  # noqa: E402
import notices  # noqa: E402
import version  # noqa: E402

BUILD = os.path.join(TOP, "build")
STAGE = os.path.join(BUILD, "dist")
STAMP = os.path.join(BUILD, "release.stamp")
NETBSD_TXT = os.path.join(TOP, "dist", "Docs", "NetBSD.txt")
BINARIES = ("AmiBSDNet", "NetCtrl", "Ping", "SerialShell", "AmiBSDNetStatus")

# a hosts file with the line for localhost
HOSTS = ("# host name table: address  name  [aliases]\n"
         "127.0.0.1  localhost\n")


def git_bash():
    """Git for Windows' bash.exe: the nearest directory at or above
    "git --exec-path" that has bin/bash.exe and usr/bin/bash.exe, else
    <ProgramFiles>/Git/bin/bash.exe.  Not the first bash on PATH: on the
    build machine that is %LOCALAPPDATA%/Microsoft/WindowsApps/bash.exe,
    a link to WSL's wsl.exe; there "git --exec-path" prints
    C:/Program Files/Git/mingw64/libexec/git-core, and C:/Program
    Files/Git has bin/bash.exe and usr/bin/bash.exe."""
    tried = []
    try:
        r = subprocess.run(["git", "--exec-path"], capture_output=True,
                           text=True, check=True)
        d = os.path.normpath(r.stdout.strip())
        while True:
            cand = os.path.join(d, "bin", "bash.exe")
            tried.append(cand)
            if (os.path.isfile(cand) and
                    os.path.isfile(os.path.join(d, "usr", "bin", "bash.exe"))):
                return cand
            up = os.path.dirname(d)
            if up == d:
                break
            d = up
    except (OSError, subprocess.CalledProcessError):
        tried.append("(git --exec-path did not run)")
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 r"C:\Program Files"):
        cand = os.path.join(root, "Git", "bin", "bash.exe")
        tried.append(cand)
        if os.path.isfile(cand):
            return cand
    sys.exit("package: Git for Windows' bash.exe was not found (tried: " +
             ", ".join(tried) + "). Install Git for Windows; no other "
             "bash is used.")


def run(cmd, env=None):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=TOP, check=True, env=env)


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def stamp_files():
    files = {n: os.path.join(BUILD, n) for n in BINARIES}
    files["NetBSD.txt"] = NETBSD_TXT
    return files


def build():
    if os.path.exists(STAMP):
        os.remove(STAMP)
    bash = git_bash()
    env = dict(os.environ, RUMP_OPT="-O2")
    for d in ("obj", "stackobj"):
        shutil.rmtree(os.path.join(BUILD, d), ignore_errors=True)
    run([sys.executable, "-I", "tools/build.py"], env)
    run([bash, "tools/build_stack.sh", "build/AmiBSDNet"])
    for src, out, extra in (
            ("src/tools/netctrl.c", "build/NetCtrl",
             ["src/common/probe.c", "src/tools/otherstacks.c",
              "src/tools/roadshow.c", "src/common/drvcheck.c"]),
            ("src/tools/ping.c", "build/Ping", []),
            ("src/tools/serialshell.c", "build/SerialShell", []),
            ("src/tools/status.c", "build/AmiBSDNetStatus",
             ["src/tools/wifiwin.c", "src/tools/settingswin.c",
              "src/common/wm.c", "src/common/probe.c",
              "src/common/drvcheck.c"])):
        run([bash, "tools/build_amiga.sh", src, out] + extra)
    notices.write(NETBSD_TXT)
    with open(STAMP, "w", newline="\n") as f:
        json.dump({"version": version.VERSION,
                   "sha256": {n: sha256(p)
                              for n, p in stamp_files().items()}},
                  f, indent=1, sort_keys=True)
        f.write("\n")


def check_stamp():
    """--no-build: only the binaries of package.py's own release build"""
    if not os.path.exists(STAMP):
        sys.exit("package: --no-build needs the binaries of a release "
                 "build by tools/package.py (build/release.stamp is "
                 "missing): run it without --no-build")
    st = json.load(open(STAMP))
    bad = [n for n, p in stamp_files().items()
           if not os.path.exists(p) or st["sha256"].get(n) != sha256(p)]
    if st.get("version") != version.VERSION:
        bad.append(f"version {st.get('version')} (now {version.VERSION})")
    if bad:
        sys.exit("package: --no-build: changed since the release build "
                 "(built again some other way?): " + ", ".join(bad) +
                 "; run tools/package.py without --no-build")


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
# generated text

def sum_var(rel):
    """the Install script's variable for a staged file, e.g.
    Docs/AmiBSDNet.txt.info -> #sum-docs-amibsdnet-txt-info"""
    return "#sum-" + re.sub(r"[^a-z0-9]+", "-", rel.lower()).strip("-")


def file_sum(path):
    """"<size> <crc32>": the CRC-32 of zlib (as src/common/drvcheck.c
    computes it), eight lower-case hex digits"""
    data = open(path, "rb").read()
    return f"{len(data)} {zlib.crc32(data) & 0xffffffff:08x}"


# staged files that get no #sum- variable: the script and its icon, and
# the icons of the package's own drawers
NOT_COPIED = ("Install", "Install.info", "Docs.info", "Status.info",
              "Internet.info")


def write_install(root):
    """dist/Install with the lines setting #ver and #sum-<file> in front;
    every #sum- variable the script uses must be a staged file, and every
    staged file it copies must have its #sum- line in the install log"""
    script = open(os.path.join(TOP, "dist", "Install"),
                  encoding="latin-1").read()
    sums = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            rel = os.path.relpath(os.path.join(dirpath, fn),
                                  root).replace(os.sep, "/")
            if rel not in NOT_COPIED:
                sums[sum_var(rel)] = file_sum(os.path.join(dirpath, fn))
    used = set(re.findall(r"#sum-[a-z0-9-]+", script))
    missing = sorted(used - set(sums))
    if missing:
        sys.exit("package: dist/Install uses " + ", ".join(missing) +
                 ", but there is no such staged file")
    unlisted = sorted(set(sums) - used)
    if unlisted:
        sys.exit("package: staged files that dist/Install does not list "
                 "in its install log: " + ", ".join(unlisted))
    head = ["; set by tools/package.py: #ver, and #sum-<file>, the size "
            "and CRC32",
            "; of each staged file",
            f'(set #ver "{version.VERSION}")']
    head += [f'(set {v} "{sums[v]}")' for v in sorted(sums)]
    with open(os.path.join(root, "Install"), "w", encoding="latin-1",
              newline="\n") as f:
        f.write("\n".join(head) + "\n\n" + script)


def write_doc(src, dst):
    """AmiBSDNet.txt with its title line, which names the version"""
    text = open(src, encoding="latin-1").read()
    title = f"AmiBSDNet {version.VERSION}"
    with open(dst, "w", encoding="latin-1", newline="\n") as f:
        f.write(f"{title}\n{'=' * len(title)}\n\n{text}")


def write_readme(src, dst, acks):
    """the readme with a Version: line (after Requires:) and, after the
    text, the acknowledgements that the advertising clauses of NetBSD
    licences require"""
    lines = open(src, encoding="latin-1").read().split("\n")
    i = next(k for k, ln in enumerate(lines) if ln.startswith("Requires:"))
    lines.insert(i + 1, f"Version:  {version.VERSION}")
    text = "\n".join(lines).rstrip("\n") + "\n"
    if acks:
        text += ("\nAcknowledgements required by the licences of the "
                 "NetBSD code\n(Docs/NetBSD.txt):\n\n")
        text += "\n".join(notices.wrap(a, "  ") for a in sorted(acks)) + "\n"
    long = [ln for ln in text.split("\n") if len(ln) > 78]
    if long:
        sys.exit(f"package: readme lines longer than 78 columns: {long}")
    with open(dst, "w", encoding="latin-1", newline="\n") as f:
        f.write(text)


def stage():
    shutil.rmtree(STAGE, ignore_errors=True)
    root = os.path.join(STAGE, "AmiBSDNet")
    for d in ("C", "Status", "Docs", "Internet"):
        os.makedirs(os.path.join(root, d))

    for name in ("AmiBSDNet", "NetCtrl", "Ping", "SerialShell"):
        shutil.copy(os.path.join(BUILD, name), os.path.join(root, "C", name))

    # third-party pieces for Wi-Fi (fetched by tools/bootstrap.py; their
    # licences are in Docs/ThirdParty.txt)
    dl = os.path.join(TOP, "downloads")
    wm = os.path.join(dl, "WirelessManager")
    if not os.path.exists(wm):
        sys.exit("package: WirelessManager missing; run "
                 "python -I tools/bootstrap.py")
    shutil.copy(wm, os.path.join(root, "C", "WirelessManager"))
    # (no Wi-Fi driver: wifipi.device and its firmware belong to the
    # Emu68 installation, so the one installed with Emu68 is used)
    shutil.copy(os.path.join(TOP, "dist", "Docs", "ThirdParty.txt"),
                os.path.join(root, "Docs", "ThirdParty.txt"))
    shutil.copy(NETBSD_TXT, os.path.join(root, "Docs", "NetBSD.txt"))
    shutil.copy(os.path.join(BUILD, "AmiBSDNetStatus"),
                os.path.join(root, "Status", "AmiBSDNetStatus"))
    write_doc(os.path.join(TOP, "dist", "Docs", "AmiBSDNet.txt"),
              os.path.join(root, "Docs", "AmiBSDNet.txt"))
    shutil.copy(os.path.join(TOP, "LICENSE"),
                os.path.join(root, "Docs", "LICENSE.txt"))
    with open(os.path.join(root, "Internet", "hosts"), "w",
              newline="\n") as f:
        f.write(HOSTS)
    write_readme(os.path.join(TOP, "dist", "AmiBSDNet.readme"),
                 os.path.join(STAGE, "AmiBSDNet.readme"),
                 notices.acknowledgements_in(NETBSD_TXT))

    mkicon.write_icon(os.path.join(STAGE, "AmiBSDNet.info"), "drawer",
                      mkicon.ART_DRAWER)
    for d in ("Docs", "Status", "Internet"):
        mkicon.write_icon(os.path.join(root, d + ".info"), "drawer",
                          mkicon.ART_DRAWER)
    mkicon.write_icon(os.path.join(root, "Install.info"), "project",
                      mkicon.ART_INSTALL, default_tool="Installer",
                      tooltypes=("APPNAME=AmiBSDNet", "MINUSER=AVERAGE",
                                 "DEFUSER=AVERAGE"))
    mkicon.write_icon(os.path.join(root, "Status", "AmiBSDNetStatus.info"),
                      "tool", mkicon.ART_STATUS, stack=8192,
                      tooltypes=("DONOTWAIT", "STACKCMD=C:AmiBSDNet",
                                 "INTERVAL=2"))
    for doc in ("AmiBSDNet.txt", "LICENSE.txt", "ThirdParty.txt",
                "NetBSD.txt"):
        mkicon.write_icon(os.path.join(root, "Docs", doc + ".info"),
                          "project", mkicon.ART_DOC,
                          default_tool="SYS:Utilities/MultiView")
    write_install(root)
    return root


def main():
    if "--no-build" in sys.argv:
        check_stamp()
    else:
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
    print(f"package: AmiBSDNet {version.VERSION}, {len(files)} files -> "
          f"{lha} ({os.path.getsize(lha)} bytes), {zp}")
    for n in BINARIES:
        print(f"  {n}: {os.path.getsize(os.path.join(BUILD, n))} bytes")


if __name__ == "__main__":
    main()
