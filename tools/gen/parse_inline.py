"""Parse an NDK-style GCC inline header (inline/<lib>.h) into
(name, lvo_offset, return_type, [(arg_type, register)]) tuples."""
import re


def parse(path):
    text = open(path, encoding="latin-1").read()
    out = []
    for m in re.finditer(r"#define (\w+)\(([^)]*)\) \(\{(.*?)\n\}\)\n", text, re.S):
        name, body = m.group(1), m.group(3)
        off = re.search(r'jsr a6@\(-(\d+):W\)', body)
        if not off:
            continue
        ret = re.search(r"register (.+?) __%s__re __asm\(\"d0\"\)" % re.escape(name), body)
        args = re.findall(r"register (.+?) __%s_(\w+) __asm\(\"(\w\d)\"\) = " % re.escape(name), body)
        out.append((name, int(off.group(1)), ret.group(1).strip() if ret else "void",
                    [(t.strip(), a, r) for t, a, r in args]))
    return sorted(out, key=lambda x: x[1])
