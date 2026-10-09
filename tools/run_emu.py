#!/usr/bin/env python3
"""Boot WinUAE with emu/hd mounted as DH0:, run a program from
S:Startup-Sequence, wait for it to write DH0:done, then close WinUAE.

Usage:  python -I tools/run_emu.py <amiga-exe> [--timeout SEC] [--rom PATH]
        [--show]

Without --rom WinUAE's built-in AROS Kickstart replacement is used.
The program's output is expected in DH0:result.txt (printed afterwards);
the startup sequence creates DH0:done once the program has returned.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = os.path.join(TOP, "emu")
HD = os.path.join(EMU, "hd")
WINUAE = os.path.join(EMU, "winuae", "winuae64.exe")

CONFIG = """\
config_description=amiga-tcpip test
use_gui=no
use_debugger=false
win32.start_not_captured=true
win32.inactive_pause=false
win32.inactive_nosound=true
win32.minimize_inactive=false
sound_output=none
nr_floppies=0
floppy0type=-1
kickstart_rom_file={rom}
chipset=aga
chipset_compatible=A4000
cpu_type=68040
cpu_model=68040
fpu_model=68040
cpu_speed=max
cpu_compatible=false
cpu_24bit_addressing=false
cachesize=8192
chipmem_size=4
fastmem_size=0
z3mem_size=128
bogomem_size=0
filesystem2=rw,DH0:Test:{hd},0
uaehf0=dir,rw,DH0:Test:{hd},0
serial_port={serial}
serial_direct=true
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--rom", default=":AROS")
    ap.add_argument("--show", action="store_true",
                    help="leave the emulator running for inspection")
    a = ap.parse_args()

    os.makedirs(os.path.join(HD, "S"), exist_ok=True)
    name = os.path.basename(a.exe)
    shutil.copy(a.exe, os.path.join(HD, name))
    for f in ("result.txt", "done", "rump.log", "stdout.txt", "serial.log"):
        p = os.path.join(HD if f != "serial.log" else EMU, f)
        if os.path.exists(p):
            os.remove(p)
    with open(os.path.join(HD, "S", "Startup-Sequence"), "w", newline="\n") as f:
        f.write(f"DH0:{name} >DH0:stdout.txt\n"
                "Echo >DH0:done \"rc=$RC\"\n")
    serial = os.path.join(EMU, "serial.log")
    cfg = os.path.join(EMU, "test.uae")
    with open(cfg, "w", newline="\n") as f:
        f.write(CONFIG.format(rom=a.rom, hd=HD, serial=serial))

    log = open(os.path.join(EMU, "winuae.log"), "w")
    proc = subprocess.Popen([WINUAE, "-portable", "-log", "-f", cfg],
                            stdout=log, stderr=subprocess.STDOUT)
    done = os.path.join(HD, "done")
    t0 = time.time()
    while time.time() - t0 < a.timeout and proc.poll() is None:
        if os.path.exists(done):
            time.sleep(0.5)
            break
        time.sleep(0.25)
    elapsed = time.time() - t0
    finished = os.path.exists(done)
    if not a.show and proc.poll() is None:
        proc.kill()
    print(f"[run_emu] {'finished' if finished else 'TIMEOUT'} after {elapsed:.1f}s")
    for f in ("done", "stdout.txt", "result.txt", "rump.log"):
        p = os.path.join(HD, f)
        if os.path.exists(p):
            with open(p, errors="replace") as fh:
                print(f"--- {f}\n{fh.read().rstrip()}")
    if os.path.exists(serial):
        with open(serial, errors="replace") as fh:
            print(f"--- serial.log\n{fh.read().rstrip()}")
    sys.exit(0 if finished else 1)


if __name__ == "__main__":
    main()
