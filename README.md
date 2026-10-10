# AmiBSDNet

A full network stack for classic AmigaOS (Workbench 3.2 and up), built
from the **NetBSD 11 kernel's networking code**: the real BSD TCP/IP,
IPv4 and IPv6 implementations rather than a reimplementation. It runs as
a [rump kernel](https://man.netbsd.org/rumpkernel.7) hosted on Exec.

Primary target: an Amiga with a PiStorm accelerator (Emu68). The code runs
on any 68020-68060; everything the stack allocates comes from Fast RAM.

## Status

Early development.

- [x] NetBSD 11 rump kernel base, sockets, routing, IPv4/IPv6 cross-built
      for m68k on Windows
- [x] `rumpuser` hypercall layer on Exec/DOS (`src/host/`)
- [x] Kernel boots under AmigaOS; TCP over the loopback interface passes
      (`src/test/rumptest.c`, run in WinUAE)
- [x] SANA-II network interface `sana0` on any SANA-II driver: TCP and
      UDP/DNS to the internet pass in WinUAE (`src/test/nettest.c`)
- [x] `bsdsocket.library` (AmiTCP/Roadshow API, 66 of 113 calls; rest
      return ENOSYS): sockets, WaitSelect, break signals, DNS resolver,
      getaddrinfo, socket passing (`src/test/socktest.c` passes)
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
- [x] Installer script for Workbench 3.2+ that detects the network
      adapters (`NetCtrl PROBE`) and other TCP/IP stacks (Roadshow,
      Miami, AmiTCP, Genesis), offers to switch from them as a trial
      (disabled reversibly, put back by itself if AmiBSDNet does not
      connect; `NetCtrl REMOVEOTHERS` removes them for good) and
      verifies they no longer start at boot; generated icons,
      LHA/ZIP packaging (`tools/package.py`)
- [x] Wi-Fi bundled: WirelessManager (WPA/WPA2) and, for the PiStorm,
      wifipi.device with firmware (third-party, see
      `dist/Docs/ThirdParty.txt`)
- [x] PaulaNET (floppy-port Wi-Fi) used automatically when plugged in,
      hidden otherwise; its driver is copied from the adapter's disk with
      read-back checking, optional verification against the official
      builds (`src/common/drvcheck.c`); not bundled (no licence)
- [x] Installed and tested with the official Workbench 3.2 installer
      (Kickstart 3.2.2, 68040) in WinUAE
- [ ] Tested on PiStorm hardware (wifipi.device, genet.device)

## Building

Windows host, native tools only (no containers or VMs): Python 3,
Git for Windows (Git Bash), 7-Zip (to unpack the WirelessManager LHA
archive from Aminet).

```sh
python -I tools/bootstrap.py           # toolchain, NetBSD sources, WinUAE,
                                       # bundled Wi-Fi files
python -I tools/build.py               # rump kernel libraries -> build/
tools/link_test.sh src/test/rumptest.c build/rumptest
python -I tools/run_emu.py build/rumptest

tools/link_test.sh src/test/nettest.c build/nettest
python -I tools/run_emu.py build/nettest --net --echo 7777

tools/build_stack.sh                   # -> build/AmiBSDNet
tools/build_amiga.sh src/test/socktest.c build/socktest
python -I tools/run_emu.py build/socktest --stack tests/slirp.conf --echo 7777

python -I tools/package.py             # release build -> build/AmiBSDNet.lha
```

`tools/run_emu.py` boots WinUAE headless with its built-in AROS Kickstart
replacement. Pass `--rom` with your own Kickstart file to test against
AmigaOS itself. ROMs are never part of this repository.

## Layout

| Path | Contents |
|---|---|
| `src/host/` | AmigaOS side: rumpuser hypercalls, debug crash handler |
| `src/kern/` | kernel-side additions (atomics, interface config, socket passing) |
| `src/lib/` | bsdsocket.library |
| `src/stack/` | the AmiBSDNet program: startup, configuration, DHCP |
| `src/include/` | headers for AmiBSDNet's own client programs |
| `src/tools/` | NetCtrl, Ping, AmiBSDNetStatus |
| `dist/` | Installer script, user documentation, Aminet readme |
| `src/test/` | test programs run in the emulator |
| `tools/` | build driver, ELF-to-hunk converter, emulator runner |
| `docs/` | design notes |

## Licence

AmiBSDNet's own code is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE). You may use, modify and
share it for any noncommercial purpose, but you may not sell it or bundle
it into a commercial product. For commercial licensing, contact the author
via GitHub.

The NetBSD sources are not part of this repository (`tools/bootstrap.py`
fetches them) and stay under NetBSD's own BSD-style licence. So do the
files here that are derived from NetBSD code, such as
`src/kern/if_virt.c`, which keep their NetBSD copyright notices.
