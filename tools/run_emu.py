#!/usr/bin/env python3
"""Boot WinUAE with emu/hd mounted as DH0:, run a test program from
S:Startup-Sequence, wait for its result, then close WinUAE.

Usage:  python -I tools/run_emu.py <amiga-exe> [options]

  --timeout SEC   give up after SEC seconds (default 60)
  --rom PATH      Kickstart ROM file (default: WinUAE's built-in AROS ROM)
  --net           enable uaenet.device (SANA-II); unit 0 is SLIRP NAT
  --echo PORT     run a TCP echo server on the host at 127.0.0.1:PORT,
                  reachable from the emulated Amiga as 10.0.2.2:PORT
  --dns PORT      run a DNS server on the host at 127.0.0.1:PORT (UDP),
                  reachable as 10.0.2.2:PORT: it answers the names
                  amibsdnet.test with 192.0.2.7 and parent.y.test with
                  192.0.2.8, and every other name with "no such name",
                  so no test needs the Internet
  --stack CONF    start build/AmiBSDNet with configuration file CONF
                  before the program (implies --net)
  --stackargs A   more arguments for build/AmiBSDNet (DEBUG, say)
  --forward PORT  forward host TCP port PORT to the Amiga (10.0.2.15:PORT)
                  and connect to it from the host, expecting an echo
  --file PATH     copy PATH to DH0: as well (repeatable)
  --args TEXT     the program's command line arguments
  --cmd LINE      a Shell command line run after the program, its output
                  appended to DH0:stdout.txt (repeatable)
  --background    start the program with Run (in the background) and
                  pass when all --cmd lines have returned
  --slow          emulate a 68030 at about 28 MHz without JIT (a classic
                  accelerator card) instead of the fastest possible 68040
  --show          leave the emulator running afterwards
  --mouse         the Amiga pointer follows the host's, so that
                  tools/uaeinput.ps1 can click in the emulator window

The host addresses: WinUAE's SLIRP (slirp/socket.cpp sosendto(), in the
WinUAE sources on GitHub) sends what the Amiga sends to 10.0.2.2 to the
host's 127.0.0.1, and what it sends to 10.0.2.3 to the host's own DNS
server.

S:Startup-Sequence starts with "FailAt 21": a return code at or above
the fail limit (10 by default) ends a script, and the commands return
5, 10 or 20 (AmigaOS manual, AmigaDOS Command Reference: FAILAT; copy in
downloads/sources/amigados-docs).  It runs the program with its output
in DH0:stdout.txt and writes its return code (the Shell's RC variable,
AmigaOS manual, Using Scripts) to DH0:rc.txt, runs the --cmd lines (each
followed by "rc=<code>" in stdout.txt) and writes DH0:end.txt last.

Test programs write DH0:done themselves, "PASS" or "FAIL", and log to
DH0:rump.log.  rumptest and nettest never return (the rump kernel's
threads keep running), so for them DH0:rc.txt never appears.  The run
passes (exit code 0) when DH0:done says PASS, the program's return code
is 0 if it returned, and, with --forward, the host got its echo back.
With --background it passes when DH0:end.txt appears.  Each --expect
text must also be in DH0:stdout.txt.  Everything is
printed afterwards.
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = os.path.join(TOP, "emu")
HD = os.path.join(EMU, "hd")
WINUAE = os.path.join(EMU, "winuae", "winuae64.exe")

# the names the --dns server knows, and their addresses (TEST-NET-1,
# RFC 5737)
DNS_NAMES = {"amibsdnet.test": "192.0.2.7", "parent.y.test": "192.0.2.8"}

# after DH0:done: how long to wait for the program to return (DH0:rc.txt)
# and for the --forward result
GRACE = 10

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


def echo_server(srv, stop):
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
                # the Amiga side closed the connection (FIN)
                print(f"[echo] closed by {peer[0]}:{peer[1]}", flush=True)
            except OSError:
                pass
    srv.close()


def dns_answer(q):
    """the reply to the DNS query q (RFC 1035 4.1); None if q is not one"""
    if len(q) < 12:
        return None
    qid, flags, qd = struct.unpack(">HHH", q[:6])
    if flags & 0x8000 or qd != 1:
        return None
    labels, p = [], 12
    while p < len(q) and q[p] != 0:
        n = q[p]
        if n > 63 or p + 1 + n > len(q):
            return None
        labels.append(q[p + 1:p + 1 + n].decode("ascii", "replace"))
        p += 1 + n
    if p + 5 > len(q):
        return None
    question = q[12:p + 5]
    qtype, qclass = struct.unpack(">HH", q[p + 1:p + 5])
    name = ".".join(labels).lower()
    rd = flags & 0x0100
    if name not in DNS_NAMES:
        # QR, RD as asked, RA, RCODE 3 (name error)
        return struct.pack(">HHHHHH", qid, 0x8080 | rd | 3, 1, 0, 0, 0) + \
            question
    answers = b""
    if qtype == 1 and qclass == 1:
        # the name as a pointer to the question's (offset 12), type A,
        # class IN, TTL 60, 4 bytes of address
        answers = (struct.pack(">HHHIH", 0xc00c, 1, 1, 60, 4) +
                   socket.inet_aton(DNS_NAMES[name]))
    # QR, AA, RD as asked, RA
    return struct.pack(">HHHHHH", qid, 0x8480 | rd, 1,
                       1 if answers else 0, 0, 0) + question + answers


def dns_server(srv, stop):
    srv.settimeout(0.5)
    while not stop.is_set():
        try:
            q, peer = srv.recvfrom(512)
        except OSError:
            continue
        r = dns_answer(q)
        if r is not None:
            srv.sendto(r, peer)
            print(f"[dns] answered {peer[0]}:{peer[1]}", flush=True)
    srv.close()


def forward_client(port, stop, result):
    """connect to the Amiga through SLIRP's port forward, expect an echo"""
    msg = b"hello from the emulator host"
    while not stop.is_set():
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as c:
                c.sendall(msg)
                got = b""
                while len(got) < len(msg):
                    d = c.recv(4096)
                    if not d:
                        break
                    got += d
                result.append("echo ok" if got == msg else f"bad echo {got!r}")
                print(f"[forward] {result[-1]}", flush=True)
                return
        except OSError:
            time.sleep(1)


def read(name):
    p = os.path.join(HD, name)
    if not os.path.exists(p):
        return None
    with open(p, errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--rom", default=":AROS")
    ap.add_argument("--net", action="store_true")
    ap.add_argument("--echo", type=int, default=0)
    ap.add_argument("--dns", type=int, default=0)
    ap.add_argument("--stack", default=None)
    ap.add_argument("--stackargs", default="")
    ap.add_argument("--file", action="append", default=[])
    ap.add_argument("--args", default="")
    ap.add_argument("--forward", type=int, default=0)
    ap.add_argument("--slow", action="store_true")
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--background", action="store_true")
    ap.add_argument("--expect", action="append", default=[])
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--mouse", action="store_true")
    a = ap.parse_args()

    # the host side first: a port that cannot be had fails the run now
    stop = threading.Event()
    try:
        if a.echo:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(("127.0.0.1", a.echo))
            srv.listen(5)
            threading.Thread(target=echo_server, args=(srv, stop),
                             daemon=True).start()
        if a.dns:
            dsrv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            dsrv.bind(("127.0.0.1", a.dns))
            threading.Thread(target=dns_server, args=(dsrv, stop),
                             daemon=True).start()
    except OSError as e:
        sys.exit(f"[run_emu] cannot open a host port: {e}")

    os.makedirs(os.path.join(HD, "S"), exist_ok=True)
    name = os.path.basename(a.exe)
    shutil.copy(a.exe, os.path.join(HD, name))
    for f in ("done", "rc.txt", "end.txt", "rump.log", "stdout.txt",
              "result.txt"):
        p = os.path.join(HD, f)
        if os.path.exists(p):
            os.remove(p)
    with open(os.path.join(HD, "S", "Startup-Sequence"), "w",
              newline="\n") as f:
        f.write("FailAt 21\n")
        if a.stack:
            a.net = True
            shutil.copy(os.path.join(TOP, "build", "AmiBSDNet"),
                        os.path.join(HD, "AmiBSDNet"))
            shutil.copy(a.stack, os.path.join(HD, "AmiBSDNet.conf"))
            shutil.copy(os.path.join(TOP, "build", "NetCtrl"),
                        os.path.join(HD, "NetCtrl"))
            f.write("DH0:AmiBSDNet CONFIG=DH0:AmiBSDNet.conf "
                    f"LOG=DH0:rump.log {a.stackargs}\n"
                    "DH0:NetCtrl WAIT TIMEOUT=60\n")
        for extra in a.file:
            shutil.copy(extra, os.path.join(HD, os.path.basename(extra)))
        if a.background:
            f.write(f"Run >DH0:stdout.txt DH0:{name} {a.args}\n")
        else:
            f.write(f"DH0:{name} {a.args} >DH0:stdout.txt\n"
                    "Echo >DH0:rc.txt \"$RC\"\n")
        for c in a.cmd:
            f.write(f"Echo >>DH0:stdout.txt \"1> {c}\"\n"
                    f"{c} >>DH0:stdout.txt\n"
                    f"Echo >>DH0:stdout.txt \"rc=$RC\"\n")
        f.write("Echo >DH0:end.txt \"end\"\n")
    cfg = os.path.join(EMU, "test.uae")
    with open(cfg, "w", newline="\n") as f:
        cfgtext = CONFIG.format(rom=a.rom, hd=HD)
        if a.slow:
            for k, v in (("cpu_type", "68030"), ("cpu_model", "68030"),
                         ("fpu_model", "68882"), ("cpu_speed", "real"),
                         ("cachesize", "0")):
                cfgtext = "\n".join(
                    f"{k}={v}" if line.startswith(k + "=") else line
                    for line in cfgtext.split("\n"))
            cfgtext += "cpu_multiplier=8\n"  # 8 x 3.55 MHz: about 28 MHz
        f.write(cfgtext)
        if a.net or a.forward:
            f.write("sana2=true\n")
        if a.forward:
            f.write(f"slirp_ports={a.forward}\n")
        if a.mouse:
            # the Amiga pointer follows the host cursor, for
            # tools/uaeinput.ps1 (the same lines as emu/install.uae)
            f.write("absolute_mouse=mousehack\nmagic_mouse=true\n")

    fwd_result = []
    if a.forward:
        threading.Thread(target=forward_client,
                         args=(a.forward, stop, fwd_result),
                         daemon=True).start()

    log = open(os.path.join(EMU, "winuae.log"), "w")
    proc = subprocess.Popen([WINUAE, "-portable", "-log", "-f", cfg],
                            stdout=log, stderr=subprocess.STDOUT)
    done = os.path.join(HD, "done")
    end = os.path.join(HD, "end.txt")
    rcfile = os.path.join(HD, "rc.txt")
    t0 = time.time()
    seen = None
    ended = None
    while time.time() - t0 < a.timeout and proc.poll() is None:
        if os.path.exists(end):
            # the --forward connection may still be under way
            ended = ended or time.time()
            if not a.forward or fwd_result or \
                    time.time() - ended >= GRACE:
                break
        if not a.background and os.path.exists(done):
            seen = seen or time.time()
            # the program may still return (rc.txt), the host's --forward
            # connection may still be under way, and the --cmd lines
            # come after the program (end.txt when they are done)
            if time.time() - seen >= GRACE or \
                    (os.path.exists(rcfile) and
                     (not a.forward or fwd_result) and not a.cmd):
                break
        time.sleep(0.25)
    time.sleep(0.5)
    elapsed = time.time() - t0
    if not a.show and proc.poll() is None:
        proc.kill()
    stop.set()

    for f in ("done", "rc.txt", "end.txt", "stdout.txt", "result.txt",
              "rump.log"):
        text = read(f)
        if text is not None:
            print(f"--- {f}\n{text.rstrip()}")
    problems = []
    if a.background:
        if not os.path.exists(end):
            problems.append("the --cmd lines did not all return in time")
    else:
        result = read("done")
        if result is None:
            problems.append("no DH0:done (the program did not finish)")
        elif not result.startswith("PASS"):
            problems.append(f"DH0:done says {result.strip()!r}")
        rc = read("rc.txt")
        if rc is not None and rc.strip() != "0":
            problems.append(f"the program returned {rc.strip()}")
    out = read("stdout.txt") or ""
    for x in a.expect:
        if x not in out:
            problems.append(f"{x!r} is not in DH0:stdout.txt")
    if a.forward and fwd_result[:1] != ["echo ok"]:
        problems.append("the host's --forward connection: " +
                        (fwd_result[0] if fwd_result else "no echo"))
    print(f"[run_emu] after {elapsed:.1f}s: " +
          ("PASS" if not problems else "FAIL: " + "; ".join(problems)))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
