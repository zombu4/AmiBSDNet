#!/usr/bin/env python3
"""Basic syntax check of an Amiga Installer script: balanced parentheses
outside strings and comments, terminated strings, no raw line breaks
inside strings (use \\n).

Usage: python -I tools/check_installer.py dist/Install
"""
import sys

BACKSLASH = chr(92)


def check(path):
    s = open(path, encoding="latin-1").read()
    depth, line, i = 0, 1, 0
    instr, strline = False, 0
    errors = []
    while i < len(s):
        c = s[i]
        if instr:
            if c == BACKSLASH:
                i += 1
            elif c == '"':
                instr = False
            elif c == "\n":
                errors.append(f"line {line}: line break inside a string "
                              f"(started on line {strline})")
        elif c == ";":
            while i < len(s) and s[i] != "\n":
                i += 1
            continue
        elif c == '"':
            instr, strline = True, line
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth < 0:
                errors.append(f"line {line}: unbalanced ')'")
                depth = 0
        if c == "\n":
            line += 1
        i += 1
    if instr:
        errors.append(f"unterminated string from line {strline}")
    if depth:
        errors.append(f"{depth} unclosed '(' at end of file")
    return errors


if __name__ == "__main__":
    errs = check(sys.argv[1])
    for e in errs:
        print(e)
    print("check_installer:", "FAIL" if errs else "ok")
    sys.exit(1 if errs else 0)
