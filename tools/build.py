#!/usr/bin/env python3
"""Feasibility-spike build driver: compile NetBSD rump kernel networking
components for m68k AmigaOS using Bartman's native Windows m68k-amiga-elf
toolchain.  No bmake / build.sh: we read SRCS / .PATH out of NetBSD's
makefiles with a deliberately small parser and handle the conditional
bits by hand (see COMPONENTS below).

Usage:  python -I tools/build.py [component ...] [-j N] [--keep-going]
"""
import argparse
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(TOP, "netbsd-src")
SYS = os.path.join(SRC, "sys")
RUMPTOP = os.path.join(SYS, "rump")
BUILD = os.path.join(TOP, "build")
TC = os.path.join(TOP, "toolchain", "opt", "bin")
CC = os.path.join(TC, "m68k-amiga-elf-gcc.exe")
NM = os.path.join(TC, "m68k-amiga-elf-nm.exe")
OBJCOPY = os.path.join(TC, "m68k-amiga-elf-objcopy.exe")
AR = os.path.join(TC, "m68k-amiga-elf-ar.exe")

CPU = os.environ.get("RUMP_CPU", "-m68040")

# ---------------------------------------------------------------------------
# minimal bmake reader


def _expand(s, var):
    def rep(m):
        name = m.group(1)
        return var.get(name, "")
    prev = None
    while prev != s:
        prev = s
        s = re.sub(r"\$\{([A-Za-z0-9_.]+)\}", rep, s)
    return s


def read_makefile(path, var, out):
    """Collect SRCS, .PATH dirs and -D/-I CPPFLAGS from unconditional lines,
    following quoted .include directives."""
    var = dict(var)
    var[".PARSEDIR"] = os.path.dirname(path)
    with open(path, encoding="latin-1") as f:
        text = f.read()
    text = re.sub(r"\\\n", " ", text)
    depth = 0
    for raw in text.split("\n"):
        line = raw.split("#", 1)[0].rstrip() if not raw.lstrip().startswith(".") else raw.rstrip()
        s = line.strip()
        if not s:
            continue
        if re.match(r"^\.\s*(if|ifdef|ifndef|ifmake|for)\b", s):
            depth += 1
            continue
        if re.match(r"^\.\s*(endif|endfor)\b", s):
            depth -= 1
            continue
        if depth:
            continue
        m = re.match(r'^\.\s*include\s+"([^"]+)"', s)
        if m:
            inc = _expand(m.group(1), var)
            if os.path.exists(inc):
                read_makefile(inc, var, out)
            continue
        m = re.match(r"^\.PATH(\.c|\.S)?:\s*(.*)$", s)
        if m:
            for d in _expand(m.group(2), var).split():
                out["path"].append(os.path.normpath(d))
            continue
        m = re.match(r"^([A-Za-z0-9_.]+)\s*([+:?]?=)\s*(.*)$", s)
        if m:
            name, op, val = m.group(1), m.group(2), _expand(m.group(3), var)
            if name == "SRCS":
                if op == "=":
                    out["srcs"] = []
                out["srcs"] += val.split()
            elif name == "CPPFLAGS":
                out["cppflags"] += [w for w in val.split() if w.startswith("-D")]
            elif op in ("=", ":="):
                var[name] = val
            elif op == "?=" and name not in var:
                var[name] = val
    return out


def component_from_makefile(mk, extra_var=None):
    var = {"RUMPTOP": RUMPTOP, ".CURDIR": os.path.dirname(mk)}
    if extra_var:
        var.update(extra_var)
    return read_makefile(mk, var, {"srcs": [], "path": [], "cppflags": []})


# ---------------------------------------------------------------------------
# components

KERNDIR = os.path.join(SYS, "lib", "libkern")
COMMON_LIBC = os.path.join(SRC, "common", "lib", "libc")


QUAD_SRCS = ("adddi3.c anddi3.c ashldi3.c ashrdi3.c cmpdi2.c divdi3.c "
             "iordi3.c lshldi3.c lshrdi3.c moddi3.c muldi3.c negdi2.c "
             "notdi2.c qdivrem.c subdi3.c ucmpdi2.c udivdi3.c umoddi3.c "
             "xordi3.c").split()


def comp_rumpkern():
    c = component_from_makefile(
        os.path.join(RUMPTOP, "librump", "rumpkern", "Makefile.rumpkern"))
    c["srcs"] += ["locks.c", "atomic_cas_generic.c"] if CPU == "-m68000" else ["locks.c"]
    gen = os.path.join(RUMPTOP, "librump", "rumpkern", "arch", "generic")
    c["path"].append(gen)
    c["srcs"] += ["rump_generic_abi.c", "rump_generic_cpu.c",
                  "rump_generic_directmap.c", "rump_generic_kobj.c",
                  "rump_generic_pmap.c"]
    # libkern + the common libc bits it pulls in, generic C only
    lk = component_from_makefile(os.path.join(KERNDIR, "Makefile.libkern"),
                                 {"KERNDIR": KERNDIR})
    c["srcs"] += lk["srcs"] + ["bswap64.c", "memset.c"]
    # no libgcc in this toolchain: NetBSD's own 64-bit arithmetic helpers
    c["srcs"] += QUAD_SRCS
    c["path"].append(os.path.join(COMMON_LIBC, "quad"))
    c["path"] += [KERNDIR]
    for d in ("atomic gen gmon inet md misc net rpc stdlib string sys "
              "hash/sha1 hash/sha2 hash/sha3 hash/rmd160 "
              "hash/murmurhash").split():
        c["path"].append(os.path.join(COMMON_LIBC, d))
    c["path"].append(os.path.join(SRC, "common", "lib", "libutil"))
    c["path"].append(os.path.join(SRC, "common", "lib", "libprop"))
    c["path"].append(os.path.join(SRC, "common", "lib", "libppath"))
    for sub in ("libutil", "libprop", "libppath"):
        mk = os.path.join(SRC, "common", "lib", sub, "Makefile.inc")
        if os.path.exists(mk):
            c["srcs"] += component_from_makefile(mk)["srcs"]
    # our own m68k/AmigaOS kernel-side code
    c["path"].append(os.path.join(TOP, "src", "kern"))
    c["srcs"] += ["atomic_m68k.c"]
    c["extra_inc"] = [os.path.join(RUMPTOP, "librump", "rumpkern")]
    return c


def comp_rumpnet():
    c = component_from_makefile(
        os.path.join(RUMPTOP, "librump", "rumpnet", "Makefile.rumpnet"))
    c["extra_inc"] = [os.path.join(RUMPTOP, "librump", "rumpkern")]
    return c


def comp_rumpnet_net():
    # libnet's Makefile pulls in libnetinet / libnetinet6 / libnetmpls
    # Makefile.inc after a bsd.init.mk include; read them explicitly.
    base = os.path.join(RUMPTOP, "net", "lib")
    c = component_from_makefile(os.path.join(base, "libnet", "Makefile"))
    c["path"].append(os.path.join(base, "libnet"))
    for sub in ("libnetinet", "libnetinet6"):
        sc = component_from_makefile(os.path.join(base, sub, "Makefile.inc"),
                                     {".CURDIR": os.path.join(base, "libnet")})
        c["srcs"] += sc["srcs"]
        c["path"] += sc["path"]
        c["cppflags"] += sc["cppflags"]
    # the PF_INET / PF_INET6 component glue (lo0 setup etc.)
    for sub in ("libnetinet", "libnetinet6"):
        c["path"].append(os.path.join(base, sub))
    c["srcs"] += ["netinet_component.c", "netinet6_component.c"]
    c["extra_inc"] = [os.path.join(RUMPTOP, "librump", "rumpkern")]
    # No IPsec: NetBSD's netinet makefiles build with -DIPSEC, but the
    # netipsec component is not part of AmiBSDNet, and raw IP sockets would
    # then call into its stubs (panic "component not available").
    c["cppflags"] = [f for f in c["cppflags"] if "IPSEC" not in f]
    c["cppflags"] += ["-DINET", "-DINET6"]
    return c


def comp_amibsdnet():
    # SANA-II network interfaces (sana0, ...): NetBSD's virtif driver with
    # our host backend (src/host/sana2.c), plus the configuration helpers
    vif = os.path.join(RUMPTOP, "net", "lib", "libvirtif")
    return {
        "srcs": ["if_virt.c", "netcfg.c", "sockpass.c"],
        "path": [vif, os.path.join(TOP, "src", "kern")],
        "cppflags": ["-DVIRTIF_BASE=sana", "-DRUMP_VIF_LINKSTR",
                     "-DINET", "-DINET6"],
        "extra_inc": [os.path.join(RUMPTOP, "librump", "rumpkern"), vif],
    }


COMPONENTS = {
    "rumpkern": comp_rumpkern,
    "rumpnet": comp_rumpnet,
    "rumpnet_net": comp_rumpnet_net,
    "amibsdnet": comp_amibsdnet,
}

# ---------------------------------------------------------------------------
# generated files and header links


def klinks():
    """bsd.klinks.mk equivalent: copy machine headers (no symlinks)."""
    kl = os.path.join(BUILD, "klinks")
    for name, src in (("machine", os.path.join(SYS, "arch", "amiga", "include")),
                      ("amiga", os.path.join(SYS, "arch", "amiga", "include")),
                      ("m68k", os.path.join(SYS, "arch", "m68k", "include"))):
        dst = os.path.join(kl, name)
        if not os.path.isdir(dst):
            shutil.copytree(src, dst)
    return kl


IOCONF_MAINBUS_H = """\
/* hand-written replacement for config(1) output: ioconf mainbus */
#ifndef IOCONF_H
#define IOCONF_H
extern struct cfdriver mainbus_cd;
#endif
"""

IOCONF_MAINBUS_C = """\
/* hand-written replacement for config(1) output: ioconf mainbus */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/mount.h>

#include "ioconf.h"

#define NORM FSTATE_NOTFOUND

static const struct cfiattrdata mainbuscf_iattrdata = {
	"mainbus", 0, {
		{ NULL, 0 },
	}
};

static const struct cfiattrdata * const mainbus_attrs[] = { &mainbuscf_iattrdata, NULL };
CFDRIVER_DECL(mainbus, DV_DULL, mainbus_attrs);

static struct cfdriver * const cfdriver_ioconf_mainbus[] = {
	&mainbus_cd,
	NULL
};

extern struct cfattach mainbus_ca;

static struct cfattach * const mainbus_cfattachinit[] = {
	&mainbus_ca, NULL
};

static const struct cfattachinit cfattach_ioconf_mainbus[] = {
	{ "mainbus", mainbus_cfattachinit },
	{ NULL, NULL }
};

static struct cfdata cfdata_ioconf_mainbus[] = {
    /* driver           attachment    unit state      loc   flags  pspec */
/*  0: mainbus0 at root */
    { "mainbus",	"mainbus",	 0, NORM,    NULL,      0, NULL },
    { NULL,		NULL,		 0,    0,    NULL,      0, NULL }
};
"""

VERS_C = """\
/* hand-written replacement for newvers.sh output */
const char copyright[] = "Copyright (c) 1996-2025 The NetBSD Foundation, Inc.\\n";
const char ostype[] = "NetBSD";
const char osrelease[] = "11.0";
const char sccs[] = "@(#)NetBSD 11.0 (RUMP-ROAST-AMIGA)";
const char version[] = "NetBSD 11.0 (RUMP-ROAST-AMIGA)\\n";
const char buildinfo[] = "";
const char kernel_ident[] = "RUMP-ROAST-AMIGA";
"""


def write_if_changed(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return
    with open(path, "w", newline="\n") as f:
        f.write(text)


def generated(name, gdir):
    if name == "rumpkern":
        write_if_changed(os.path.join(gdir, "ioconf.h"), IOCONF_MAINBUS_H)
        write_if_changed(os.path.join(gdir, "ioconf.c"), IOCONF_MAINBUS_C)
        write_if_changed(os.path.join(gdir, "vers.c"), VERS_C)
    elif name == "rumpnet_net":
        write_if_changed(os.path.join(gdir, "ioconf.h"),
                         "/* ioconf net */\nvoid carpattach(int);\n")


# ---------------------------------------------------------------------------
# compile


def find_src(name, paths, gdir):
    for d in [gdir] + paths:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def cflags(c, gdir, kl):
    inc = [os.path.join(RUMPTOP, "include")] + c.get("extra_inc", []) + [
        gdir, kl,
        os.path.join(SRC, "common", "include"),
        os.path.join(RUMPTOP, "include", "opt"),
        os.path.join(SYS, "arch"),
        SYS,
        os.path.join(COMMON_LIBC, "quad"),
        os.path.join(COMMON_LIBC, "string"),
        os.path.join(COMMON_LIBC, "hash", "sha3"),
        os.path.join(SRC, "common", "include", "libc"),
        KERNDIR,
    ]
    # RUMP_OPT overrides optimisation/debug flags, e.g. "-Os -ffunction-sections"
    opt = os.environ.get("RUMP_OPT", "-O2 -DDIAGNOSTIC").split()
    f = [CPU] + opt + [
         # natural (4-byte) alignment for 32-bit types: NetBSD asserts it
         # for atomically accessed pointers.  Kernel side only; host code
         # keeps the AmigaOS ABI's 2-byte alignment for NDK structures.
         "-malign-int",
         # keep build-machine paths (and user names) out of __FILE__
         f"-ffile-prefix-map={SRC}=netbsd-src",
         f"-ffile-prefix-map={TOP}=.",
         "-g0", "-ffreestanding", "-fno-strict-aliasing",
         "-fno-delete-null-pointer-checks", "-fno-common", "-nostdinc",
         "-std=gnu99", "-w",
         "-D_KERNEL", "-D_RUMPKERNEL", "-D__NetBSD__=1",
         "-DRUMP_USE_CTOR", "-DRUMP_CURLWP=RUMP_CURLWP_HYPERCALL",
         "-DMAXUSERS=32",
         "-imacros", os.path.join(RUMPTOP, "include", "opt", "opt_rumpkernel.h")]
    # Bartman's m68k-amiga-elf GCC follows the Amiga convention of 32-bit
    # types being 'long'; NetBSD/m68k (via sys/common_int_types.h) expects
    # 'int'.  Same size and ABI, but the C types must agree.
    for m, t in (("__INT32_TYPE__", "int"), ("__UINT32_TYPE__", "unsigned int"),
                 ("__INT_LEAST32_TYPE__", "int"),
                 ("__UINT_LEAST32_TYPE__", "unsigned int"),
                 ("__INTPTR_TYPE__", "int"), ("__UINTPTR_TYPE__", "unsigned int"),
                 ("__PTRDIFF_TYPE__", "int"), ("__SIZE_TYPE__", "unsigned int"),
                 ("__WCHAR_TYPE__", "int")):
        f += [f"-U{m}", f"-D{m}={t}"]
    f += sorted(set(c["cppflags"]))
    f += ["-I" + i for i in inc]
    return f


# per-file extra flags (NetBSD's COPTS.<file>)
COPTS = {
    # tentative definitions shadowing the real ones in net/if.c
    "net_stub.c": ["-fcommon"],
}


def compile_one(src, obj, flags):
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    flags = flags + COPTS.get(os.path.basename(src), [])
    r = subprocess.run([CC] + flags + ["-c", src, "-o", obj],
                       capture_output=True, text=True)
    return src, obj, r.returncode, r.stderr


def rename_symbols(obj):
    """Makefile.rump rump_symren: prefix every global symbol not already in
    the rump namespace with rumpns_."""
    r = subprocess.run([NM, "-P", "-g", obj], capture_output=True, text=True)
    pairs = []
    for line in r.stdout.splitlines():
        sym = line.split()[0]
        if re.match(r"^(rump|RUMP|__|_GLOBAL_OFFSET_TABLE)", sym):
            continue
        pairs.append(f"{sym} rumpns_{sym}\n")
    if pairs:
        mp = obj + ".ren"
        with open(mp, "w") as f:
            f.writelines(pairs)
        subprocess.run([OBJCOPY, "--preserve-dates", "--redefine-syms", mp, obj],
                       check=True)
        os.remove(mp)


def build(name, jobs, keep_going):
    c = COMPONENTS[name]()
    gdir = os.path.join(BUILD, "gen", name)
    odir = os.path.join(BUILD, "obj", name)
    generated(name, gdir)
    kl = klinks()
    flags = cflags(c, gdir, kl)
    srcs, missing, seen = [], [], set()
    for s in c["srcs"]:
        if s in seen or not s.endswith(".c"):
            continue
        seen.add(s)
        p = find_src(s, c["path"], gdir)
        (srcs.append(p) if p else missing.append(s))
    norename = {"rump_syscalls.c"}
    fails = []
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = [ex.submit(compile_one, s,
                          os.path.join(odir, os.path.basename(s)[:-2] + ".o"),
                          flags) for s in srcs]
        for fu in cf.as_completed(futs):
            src, obj, rc, err = fu.result()
            if rc:
                fails.append((src, err))
            elif os.path.basename(src) not in norename:
                rename_symbols(obj)
    objs = [os.path.join(odir, os.path.basename(s)[:-2] + ".o") for s in srcs]
    objs = [o for o in objs if os.path.exists(o)]
    lib = os.path.join(BUILD, f"lib{name}.a")
    if os.path.exists(lib):
        os.remove(lib)
    if objs:
        subprocess.run([AR, "rcs", lib] + objs, check=True)
    logp = os.path.join(BUILD, f"{name}.errors.log")
    with open(logp, "w") as f:
        for src, err in sorted(fails):
            f.write(f"==== {os.path.relpath(src, SRC)}\n{err}\n")
    print(f"[{name}] sources={len(srcs)} ok={len(srcs) - len(fails)} "
          f"failed={len(fails)} missing={len(missing)}")
    if missing:
        print(f"[{name}] missing: {' '.join(missing)}")
    if fails:
        print(f"[{name}] errors in {logp}")
    return not fails and not missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("components", nargs="*", default=list(COMPONENTS))
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--keep-going", action="store_true")
    a = ap.parse_args()
    ok = True
    for n in a.components:
        ok &= build(n, a.j, a.keep_going)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
