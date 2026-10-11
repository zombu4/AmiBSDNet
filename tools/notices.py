#!/usr/bin/env python3
"""Collect the copyright and licence notices of the NetBSD code compiled
into AmiBSDNet, for the binary package (Docs/NetBSD.txt).

NetBSD's licences require binary redistributions to reproduce the
copyright notice, the conditions and the disclaimer (for example
netbsd-src/sys/netinet/tcp_input.c, clause 2 of each of its licences).

The list of compiled files comes from the compiler's dependency files
(-MD): build/obj/<component>/*.d (tools/build.py) and build/stackobj/*.d
(tools/build_stack.sh), so it holds every source and header that went
into the kernel libraries and the stack program.  Every comment block in
them that carries a copyright and licence text is collected; identical
notices are listed once, with the files they come from.  Notices with
an advertising clause ("All advertising materials mentioning features or
use of this software must display the following acknowledgement") have
their acknowledgements listed at the top as well.

Usage: python -I tools/notices.py [output]   (default dist/Docs/NetBSD.txt)
"""
import glob
import os
import re
import sys
import textwrap

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(TOP, "build")
OUT = os.path.join(TOP, "dist", "Docs", "NetBSD.txt")

# build/klinks holds copies of NetBSD machine headers (tools/build.py
# klinks()): named after the NetBSD file they were copied from
KLINKS = {"machine": "sys/arch/amiga/include", "amiga": "sys/arch/amiga/include",
          "m68k": "sys/arch/m68k/include"}

COMMENT = re.compile(r"/\*.*?\*/", re.S)
WIDTH = 78


def dep_files():
    return sorted(glob.glob(os.path.join(BUILD, "obj", "*", "*.d")) +
                  glob.glob(os.path.join(BUILD, "stackobj", "*.d")))


def parse_dep(path):
    """the prerequisites of a make rule as written by gcc -MD"""
    text = open(path, encoding="utf-8", errors="surrogateescape").read()
    text = text.replace("\\\n", " ")
    out = []
    for line in text.split("\n"):
        # "target: dep dep ..." (a Windows drive letter is "c:/", so split
        # at the first ": ")
        i = line.find(": ")
        if i < 0 or line.startswith("\t"):
            continue
        # "\ " is a blank inside a file name
        for d in re.split(r"(?<!\\)\s+", line[i + 2:].strip()):
            if d:
                out.append(d.replace("\\ ", " "))
    return out


def label(path):
    """the name a file is listed under; None for files not to scan"""
    p = os.path.normpath(os.path.join(TOP, path))
    rel = os.path.relpath(p, TOP).replace(os.sep, "/")
    if rel.startswith("netbsd-src/"):
        return rel[len("netbsd-src/"):]
    if rel.startswith("build/klinks/"):
        parts = rel.split("/", 3)
        if len(parts) == 4 and parts[2] in KLINKS:
            return KLINKS[parts[2]] + "/" + parts[3]
    if rel.startswith("src/"):
        return "AmiBSDNet " + rel
    return None


def read_text(path):
    data = open(path, "rb").read()
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("latin-1")


def clean(block):
    """a comment block as plain text: no comment markers or ' * '"""
    lines = block[2:-2].split("\n")
    if lines and lines[0].strip() in ("", "-"):
        lines = lines[1:]
    out = []
    for ln in lines:
        s = ln.rstrip()
        m = re.match(r"^\s*\* ?", s)
        if m:
            s = s[m.end():]
        out.append(s.expandtabs(8).rstrip())
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out)


def is_notice(text, ours):
    low = text.lower()
    if ours:
        # AmiBSDNet's own files: only the NetBSD-derived ones
        return "redistribution and use" in low
    if "copyright" not in low:
        return False
    return any(w in low for w in ("redistribution", "permission to use",
                                  "permission is hereby granted",
                                  "grants permission", "licen"))


def acknowledgements(text):
    """the sentences an advertising clause requires to be shown"""
    if "advertising materials" not in text.lower():
        return []
    flat = " ".join(text.split())
    m = re.search(r"must display the following acknowledge?ments?:?\s*(.*?)"
                  r"(?=\s\d\.\s|\sTHIS SOFTWARE|\sTHE SOFTWARE"
                  r"|\sRedistribution and use|$)", flat,
                  re.I)
    if not m:
        return []
    found = [x.strip() for x in
             re.split(r"(?=This product includes )", m.group(1))
             if x.strip()]
    return found


def collect():
    deps = dep_files()
    if not deps:
        sys.exit("notices: no dependency files in build/obj or "
                 "build/stackobj: build first (tools/build.py, "
                 "tools/build_stack.sh)")
    files = {}
    for d in deps:
        for f in parse_dep(d):
            name = label(f)
            full = os.path.join(TOP, f)
            if name and os.path.isfile(full):
                files.setdefault(name, full)
    notices = {}         # normalised text -> [text, set of files]
    acks = {}
    for name in sorted(files):
        ours = name.startswith("AmiBSDNet ")
        for block in COMMENT.findall(read_text(files[name])):
            text = clean(block)
            if not is_notice(text, ours):
                continue
            key = " ".join(text.split())
            notices.setdefault(key, [text, set()])[1].add(name)
            for a in acknowledgements(text):
                acks.setdefault(" ".join(a.split()), set()).add(name)
    return files, notices, acks


def wrap(text, indent=""):
    return textwrap.fill(text, WIDTH, initial_indent=indent,
                         subsequent_indent=indent, break_on_hyphens=False,
                         break_long_words=False)


def render(files, notices, acks):
    out = ["NetBSD copyright and licence notices",
           "====================================", ""]
    out.append(wrap(
        "AmiBSDNet contains code from the NetBSD operating system "
        "(https://www.NetBSD.org), compiled from the NetBSD 11 sources. "
        "Their licences require that binary copies reproduce the "
        "copyright notices, the lists of conditions and the disclaimers "
        "of that code: they follow, for every NetBSD source and header "
        f"compiled into this AmiBSDNet release ({len(files)} files). Each "
        "notice is listed once, followed by the files it comes from."))
    out.append("")
    if acks:
        out += ["Acknowledgements", "----------------"]
        out.append(wrap(
            "Some of these licences require all advertising materials "
            "mentioning features or use of the software to display the "
            "following acknowledgements:"))
        out.append("")
        for a in sorted(acks):
            out.append(wrap(a, "  "))
        out.append("")
    out += ["Notices", "-------"]
    order = sorted(notices.values(),
                   key=lambda v: (-len(v[1]), sorted(v[1])[0]))
    for i, (text, names) in enumerate(order, 1):
        out += ["", f"[{i}]", text, ""]
        out.append(wrap("From: " + ", ".join(sorted(names)), "  "))
    out.append("")
    return "\n".join(out)


def acknowledgements_in(path):
    """the acknowledgements listed in a file written by write()"""
    lines = open(path, encoding="latin-1").read().split("\n")
    try:
        i = lines.index("Acknowledgements")
    except ValueError:
        return []
    out, cur = [], None
    for ln in lines[i + 2:]:
        if ln == "Notices":
            break
        if ln.startswith("  This product includes "):
            if cur:
                out.append(cur)
            cur = ln.strip()
        elif ln.startswith("  ") and cur:
            cur += " " + ln.strip()
        elif not ln.strip() and cur:
            out.append(cur)
            cur = None
    if cur:
        out.append(cur)
    return out


def write(path=OUT):
    files, notices, acks = collect()
    text = render(files, notices, acks)
    # Latin-1 for the Amiga; a character outside it (a name in a
    # notice) is written as its code point, <U+XXXX>
    lost = sum(1 for c in text if ord(c) > 255)
    data = "".join(c if ord(c) < 256 else f"<U+{ord(c):04X}>"
                   for c in text).encode("latin-1")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print(f"notices: {len(notices)} notices from {len(files)} files, "
          f"{len(acks)} acknowledgements -> {path}")
    if lost:
        print(f"notices: {lost} characters outside Latin-1 written as "
              f"<U+XXXX>")
    return path, acks


if __name__ == "__main__":
    write(sys.argv[1] if len(sys.argv) > 1 else OUT)
