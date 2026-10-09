# AmiBSDNet

A full network stack for classic AmigaOS (Workbench 3.2 and up), built
from the **NetBSD 11 kernel's networking code**: the real BSD TCP/IP,
IPv4 and IPv6 implementations rather than a reimplementation. It runs as
a [rump kernel](https://man.netbsd.org/rumpkernel.7) hosted on Exec.

Primary target: an Amiga with a PiStorm accelerator (Emu68, 68040 code,
plenty of Fast RAM). Everything the stack allocates comes from Fast RAM.

## Status

Early development.

- [x] NetBSD 11 rump kernel base, sockets, routing, IPv4/IPv6 cross-built
      for m68k on Windows
- [x] `rumpuser` hypercall layer on Exec/DOS (`src/host/`)
- [x] Kernel boots under AmigaOS; TCP over the loopback interface passes
      (`src/test/rumptest.c`, run in WinUAE)
- [x] SANA-II network interface `sana0` on any SANA-II driver: TCP and
      UDP/DNS to the internet pass in WinUAE (`src/test/nettest.c`)
- [ ] `bsdsocket.library` so existing Amiga network software works
- [ ] Starts at boot; configuration (DHCP, static, DNS)
- [ ] Workbench status AppIcon / Commodity
- [ ] Installer script for Workbench 3.2+

## Building

Windows host, native tools only (no containers or VMs): Python 3,
Git for Windows (Git Bash).

```sh
python -I tools/bootstrap.py           # toolchain, NetBSD sources, WinUAE
python -I tools/build.py               # rump kernel libraries -> build/
tools/link_test.sh src/test/rumptest.c build/rumptest
python -I tools/run_emu.py build/rumptest

tools/link_test.sh src/test/nettest.c build/nettest
python -I tools/run_emu.py build/nettest --net --echo 7777
```

`tools/run_emu.py` boots WinUAE headless with its built-in AROS Kickstart
replacement. Pass `--rom` with your own Kickstart file to test against
AmigaOS itself. ROMs are never part of this repository.

## Layout

| Path | Contents |
|---|---|
| `src/host/` | AmigaOS side: rumpuser hypercalls, debug crash handler |
| `src/kern/` | kernel-side additions (m68k atomic operations) |
| `src/test/` | test programs run in the emulator |
| `tools/` | build driver, ELF-to-hunk converter, emulator runner |
| `docs/` | design notes |

## Licence

AmiBSDNet's own code is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE). You may use, modify and
share it for any noncommercial purpose, but you may not sell it or bundle
it into a commercial product. For commercial licensing, contact the author
via GitHub.

NetBSD code is not part of this repository (`tools/bootstrap.py` fetches
it) and stays under NetBSD's own BSD-style licence.
