#!/usr/bin/env python3
"""Boot WinUAE with emu/hd mounted as DH0:, run a program from
S:Startup-Sequence and wait until DH0:done appears, then close WinUAE.

Usage:  python -I tools/run_emu.py <amiga-exe> [options]

  --timeout SEC   give up after SEC seconds (default 60)
  --rom PATH      Kickstart ROM file (default: WinUAE's built-in AROS ROM)
  --net           enable uaenet.device (SANA-II); unit 0 is SLIRP NAT
  --echo PORT     run a TCP echo server on the host at 127.0.0.1:PORT,
                  reachable from the emulated Amiga as 10.0.2.2:PORT
  --stack CONF    start build/AmiBSDNet with configuration file CONF
                  before the program (implies --net)
  --show          leave the emulator running afterwards

Test programs write DH0:done themselves and log to DH0:rump.log; both,
and the program's Shell output, are printed afterwards.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = os.path.join(TOP, "emu")
HD = os.path.join(EMU, "hd")
WINUAE = os.path.join(EMU, "winuae", "winuae64.exe")

CONFIG = """\
config_description=AmiBSDNet test
use_gui=no
use_debugger=false
win32.start_not_captured=true
win32.inactive_pause=false
win32.inactive_nosound=true
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
"""


def echo_server(port, stop):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    srv.settimeout(0.5)
    while not stop.is_set():
        try:
            conn, peer = srv.accept()
        except socket.timeout:
            continue
        print(f"[echo] connection from {peer[0]}:{peer[1]}", flush=True)
        with conn:
            conn.settimeout(10)
            try:
                while data := conn.recv(4096):
                    conn.sendall(data)
            except OSError:
                pass
    srv.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--rom", default=":AROS")
    ap.add_argument("--net", action="store_true")
    ap.add_argument("--echo", type=int, default=0)
    ap.add_argument("--stack", default=None)
    ap.add_argument("--file", action="append", default=[])
    ap.add_argument("--args", default="")
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--show", action="store_true")
    a = ap.parse_args()

    os.makedirs(os.path.join(HD, "S"), exist_ok=True)
    name = os.path.basename(a.exe)
    shutil.copy(a.exe, os.path.join(HD, name))
    for f in ("done", "rump.log", "stdout.txt", "result.txt"):
        p = os.path.join(HD, f)
        if os.path.exists(p):
            os.remove(p)
    with open(os.path.join(HD, "S", "Startup-Sequence"), "w",
              newline="\n") as f:
        if a.stack:
            a.net = True
            shutil.copy(os.path.join(TOP, "build", "AmiBSDNet"),
                        os.path.join(HD, "AmiBSDNet"))
            shutil.copy(a.stack, os.path.join(HD, "AmiBSDNet.conf"))
            shutil.copy(os.path.join(TOP, "build", "NetCtrl"),
                        os.path.join(HD, "NetCtrl"))
            f.write("DH0:AmiBSDNet CONFIG=DH0:AmiBSDNet.conf "
                    "LOG=DH0:rump.log\n"
                    "DH0:NetCtrl WAIT TIMEOUT=60\n")
        for extra in a.file:
            shutil.copy(extra, os.path.join(HD, os.path.basename(extra)))
        f.write(f"DH0:{name} {a.args} >DH0:stdout.txt\n")
        for c in a.cmd:
            f.write(f"Echo >>DH0:stdout.txt \"1> {c}\"\n"
                    f"{c} >>DH0:stdout.txt\n")
        f.write("Echo >DH0:done \"rc=$RC\"\n")
    cfg = os.path.join(EMU, "test.uae")
    with open(cfg, "w", newline="\n") as f:
        f.write(CONFIG.format(rom=a.rom, hd=HD))
        if a.net:
            f.write("sana2=true\n")

    stop = threading.Event()
    if a.echo:
        threading.Thread(target=echo_server, args=(a.echo, stop),
                         daemon=True).start()

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
    stop.set()
    print(f"[run_emu] {'finished' if finished else 'TIMEOUT'} "
          f"after {elapsed:.1f}s")
    for f in ("done", "stdout.txt", "result.txt", "rump.log"):
        p = os.path.join(HD, f)
        if os.path.exists(p):
            with open(p, errors="replace") as fh:
                print(f"--- {f}\n{fh.read().rstrip()}")
    sys.exit(0 if finished else 1)


if __name__ == "__main__":
    main()
