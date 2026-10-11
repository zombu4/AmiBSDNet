# AmiBSDNet

A full network stack for classic AmigaOS (AmigaOS 3.1 and up), built
from the **NetBSD 11 kernel's networking code**: the real BSD TCP/IP,
IPv4 and IPv6 implementations rather than a reimplementation. It runs as
a [rump kernel](https://man.netbsd.org/rumpkernel.7) hosted on Exec.

Primary target: an Amiga with a PiStorm accelerator (Emu68). The code is
compiled for the 68020-68060 (`-m68020-60`, no FPU instructions);
everything the stack allocates comes from Fast RAM. So far it has been
tested in WinUAE only (see Status).

## Status

Early development.

- [x] NetBSD 11 rump kernel base, sockets, routing, IPv4/IPv6 cross-built
      for m68k on Windows
- [x] `rumpuser` hypercall layer on Exec/DOS (`src/host/`)
- [x] Kernel boots under AmigaOS; TCP over the loopback interface passes
      (`src/test/rumptest.c`, run in WinUAE)
- [x] SANA-II network interface `sana0` on any SANA-II driver: TCP and
      UDP/DNS pass in WinUAE against servers on the emulator host
      (`src/test/nettest.c`)
- [x] `bsdsocket.library` (AmiTCP/Roadshow API: the 113 calls of the
      SDK's inline header; the other 20 vector slots, ChangeRouteTagList
      among them, return ENOSYS; the packet filter has 0 channels, so
      bpf_* fail with ENXIO, `src/lib/bpf.c`): sockets, WaitSelect,
      break signals, DNS resolver, getaddrinfo, socket passing, the
      interface, route, monitoring and mbuf calls
      (`src/test/socktest.c`, `src/test/libtest.c` pass)
- [x] Stack program `AmiBSDNet`: detaches, reads a config file, DHCP
      client with renewal, static addresses, routes, DNS
- [x] Control port + `NetCtrl` (STATUS/ONLINE/OFFLINE/RECONFIG), `Ping`
- [x] Starts at boot from S:User-Startup without delaying it
      (`NetCtrl WAIT` for scripts that need the network)
- [x] Workbench status AppIcon + Exchange commodity (`AmiBSDNetStatus`),
      with a Wi-Fi window (scan, passphrase, connect) and a Settings
      window (drivers with Detect, DHCP or fixed address, DNS, host name,
      Wi-Fi network) that applies changes without a reboot; both also in
      the Workbench Tools menu
- [x] Ethernet and Wi-Fi at the same time, default route via Ethernet
      while connected, Wi-Fi otherwise
- [x] Link detection (SANA-II link events) and a DHCP client that keeps
      retrying, so cables/routers appearing after boot are picked up;
      WirelessManager started automatically for Wi-Fi interfaces
- [x] Installer script (`dist/Install`) that detects the network
      adapters (`NetCtrl PROBE`) and other TCP/IP stacks (Roadshow,
      Miami, AmiTCP, Genesis), offers to switch from them as a trial
      (the default: disabled reversibly, put back by itself if AmiBSDNet
      does not connect; removing Roadshow completely is a separate
      question whose default is No; `NetCtrl REMOVEOTHERS` removes other
      stacks' startup entries for good) and verifies they no longer start
      at boot; generated icons, LHA/ZIP packaging (`tools/package.py`)
- [x] WirelessManager (WPA/WPA2) bundled (third-party, see
      `dist/Docs/ThirdParty.txt`); the PiStorm's wifipi.device and its
      firmware are never touched (they belong to the Emu68 installation)
- [x] PaulaNET (floppy-port Wi-Fi) used automatically when plugged in,
      hidden otherwise; its driver is copied from the adapter's disk with
      read-back checking, optional verification against the official
      builds (`src/common/drvcheck.c`); not bundled (no licence)
- [x] `SerialShell`: a Shell on the serial port (8N1, no handshaking,
      19200 baud by default), switched on and off in Settings
- [x] Start notice: for the first minute after a boot each program shows
      the step it is about to take on screen (a freeze leaves the culprit
      visible); a start that never finished is skipped once at the next
      boot (`src/include/amibsdnet/notice.h`)
- [x] Uninstaller (the installer's "Uninstall", `NetCtrl UNINSTALL`):
      removes what the install log `S:AmiBSDNet-Install.log` lists, then
      the log; drawers only if nothing else is in them
- [x] Installed and tested with the official Workbench 3.2 installer
      (Kickstart 3.2.2, 68040) in WinUAE
- [ ] Tested on PiStorm hardware (wifipi.device, genet.device), with
      PaulaNET or with Zorro network cards: not yet

## Building

Windows host, native tools only (no containers or VMs): Python 3,
Git for Windows (Git Bash), 7-Zip (to unpack the WirelessManager LHA
archive from Aminet).

```sh
python -I tools/bootstrap.py           # toolchain, NetBSD sources, WinUAE,
                                       # WirelessManager
python -I tools/build.py               # rump kernel libraries -> build/
tools/link_test.sh src/test/rumptest.c build/rumptest
python -I tools/run_emu.py build/rumptest

tools/link_test.sh src/test/nettest.c build/nettest
python -I tools/run_emu.py build/nettest --net --echo 7777 --dns 53

tools/build_stack.sh                   # -> build/AmiBSDNet
tools/build_amiga.sh src/tools/netctrl.c build/NetCtrl src/common/probe.c \
    src/tools/otherstacks.c src/tools/roadshow.c src/common/drvcheck.c
tools/build_amiga.sh src/test/socktest.c build/socktest
python -I tools/run_emu.py build/socktest --stack tests/slirp-static.conf \
    --echo 7777 --dns 53
tools/build_amiga.sh src/test/libtest.c build/libtest
python -I tools/run_emu.py build/libtest --stack tests/slirp-static.conf \
    --echo 7777 --dns 53
tools/build_amiga.sh src/test/rmiftest.c build/rmiftest
python -I tools/run_emu.py build/rmiftest --stack tests/slirp-static.conf
tools/build_amiga.sh src/test/srvtest.c build/srvtest
python -I tools/run_emu.py build/srvtest --stack tests/slirp.conf --forward 2323
tools/build_amiga.sh src/test/wmtest.c build/wmtest src/common/wm.c
python -I tools/run_emu.py build/wmtest
tools/build_amiga.sh src/test/uninsttest.c build/uninsttest
python -I tools/run_emu.py build/uninsttest --file build/NetCtrl
tools/build_amiga.sh src/test/rstest.c build/rstest
python -I tools/run_emu.py build/rstest --file build/NetCtrl
tools/build_amiga.sh src/test/paniktest.c build/paniktest
python -I tools/run_emu.py build/paniktest --stack tests/slirp-static.conf \
    --stackargs DEBUG --file build/NetCtrl --args TASK
python -I tools/run_emu.py build/paniktest --stack tests/slirp-static.conf \
    --stackargs DEBUG --file build/NetCtrl --args THREAD
python -I tools/run_emu.py build/paniktest --stack tests/slirp-static.conf \
    --file build/NetCtrl --args NODEBUG
tools/build_amiga.sh src/test/hangport.c build/hangport
python -I tools/run_emu.py build/hangport --background --file build/NetCtrl \
    --cmd "DH0:NetCtrl STATUS" --timeout 120 \
    --expect "NetCtrl: AmiBSDNet does not answer" --expect "rc=5"

python -I tools/check_installer.py dist/Install   # Installer script check

python -I tools/package.py             # release build -> build/AmiBSDNet.lha
```

`tools/run_emu.py` starts WinUAE without its configuration GUI (the
emulator window still opens) with its built-in AROS Kickstart
replacement, and exits with 0 only if the test passed. Pass `--rom` with
your own Kickstart file to test against AmigaOS itself. ROMs are never
part of this repository. The tests need no Internet: `--echo` and
`--dns` run an echo server and a DNS server on the host, which the
emulated Amiga reaches as 10.0.2.2.

The version is set in `tools/version.py`; `tools/package.py` puts it
into the packaged installer, documentation and readme, and
`tools/version.py` writes `build/gen/amibsdnet_version.h` for the C
programs.

## Layout

| Path | Contents |
|---|---|
| `src/host/` | AmigaOS side: rumpuser hypercalls, SANA-II backend, debug crash handler |
| `src/kern/` | kernel-side additions (atomics, interface config, socket passing) |
| `src/lib/` | bsdsocket.library |
| `src/stack/` | the AmiBSDNet program: startup, configuration, DHCP |
| `src/common/` | code shared by the stack and the tools (adapter probe, PaulaNET driver check, WirelessManager control) |
| `src/include/` | headers for AmiBSDNet's own client programs |
| `src/tools/` | NetCtrl, Ping, SerialShell, AmiBSDNetStatus |
| `src/test/` | test programs run in the emulator (`hangport.c`: a stack that never answers, for the client timeouts) |
| `tests/` | stack configurations for the emulator tests |
| `dist/` | Installer script, user documentation (`Docs/`, with the NetBSD licence notices in `Docs/NetBSD.txt`), readme |
| `tools/` | build driver, ELF-to-hunk converter, emulator runner, packaging, licence notice collector, `uaeinput.ps1` (clicks and screenshots in WinUAE) |
| `tools/gen/` | generators for the bsdsocket.library vectors and inline calls |

## Licence

AmiBSDNet's own code is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE). You may use, modify and
share it for any noncommercial purpose, but you may not sell it or bundle
it into a commercial product. For commercial licensing, contact the author
via GitHub.

The NetBSD sources are not part of this repository (`tools/bootstrap.py`
fetches them) and stay under NetBSD's own BSD-style licences. So do the
files here that are derived from NetBSD code, such as
`src/kern/if_virt.c`, which keep their NetBSD copyright notices. The
binary package carries the notices of all NetBSD code compiled into it
(`dist/Docs/NetBSD.txt`, collected by `tools/notices.py`).
