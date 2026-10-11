#!/usr/bin/env python3
"""Check an Amiga Installer script.

Against the Installer 43.3 guide (Installer.guide in Aminet
util/misc/Installer-43_3.lha):
- balanced parentheses outside strings ("..." and '...') and comments
  (';' to the end of the line), terminated strings;
- "and", "or" and "xor" with exactly two arguments (the guide documents
  "(AND <expression-1> <expression-2>)" and no other form);
- no askchoice: the guide says it returns a bit mask of the choices,
  while its "default" is the number of a choice, so its result is not
  documented unambiguously (askbool returns 0 or 1);
- only the pre-defined @ variables the guide lists.
This project's own style: no line break inside a string (\\n is used).

Usage: python -I tools/check_installer.py dist/Install
"""
import sys

BACKSLASH = chr(92)

# "Pre-Defined Variables" in Installer.guide
VARIABLES = {
    "@abort-button", "@app-name", "@askchoice-help", "@askdir-help",
    "@askdisk-help", "@askfile-help", "@asknumber-help",
    "@askoptions-help", "@askstring-help", "@copyfiles-help",
    "@copylib-help", "@default-dest", "@each-name", "@each-type",
    "@error-msg", "@execute-dir", "@icon", "@installer-version", "@ioerr",
    "@language", "@makedir-help", "@pretend", "@special-msg",
    "@startup-help", "@user-level",
}
TWO_ARGS = {"and", "or", "xor"}


def tokens(s, errors):
    """(kind, text, line): kind "(", ")", "str" or "atom"; checks the
    lexical rules on the way"""
    out = []
    line, i, depth = 1, 0, 0
    while i < len(s):
        c = s[i]
        if c == ";":
            while i < len(s) and s[i] != "\n":
                i += 1
            continue
        if c in "\"'":
            quote, start = c, line
            i += 1
            while i < len(s) and s[i] != quote:
                if s[i] == BACKSLASH:
                    i += 1
                elif s[i] == "\n":
                    errors.append(f"line {line}: line break inside a string "
                                  f"(started on line {start})")
                    line += 1
                i += 1
            if i >= len(s):
                errors.append(f"unterminated string from line {start}")
            out.append(("str", "", start))
            i += 1
            continue
        if c == "(":
            depth += 1
            out.append(("(", c, line))
        elif c == ")":
            depth -= 1
            if depth < 0:
                errors.append(f"line {line}: unbalanced ')'")
                depth = 0
            else:
                out.append((")", c, line))
        elif c == "\n":
            line += 1
        elif not c.isspace():
            j = i
            while (j < len(s) and not s[j].isspace() and
                   s[j] not in "();\"'"):
                j += 1
            out.append(("atom", s[i:j], line))
            i = j
            continue
        i += 1
    if depth:
        errors.append(f"{depth} unclosed '(' at end of file")
    return out


def check(path):
    s = open(path, encoding="latin-1").read()
    errors = []
    # each open "(": [first atom, number of items, line]
    stack = []
    for kind, text, line in tokens(s, errors):
        if kind == "(":
            stack.append([None, 0, line])
            continue
        if kind == ")":
            if not stack:
                continue
            head, n, start = stack.pop()
            if head in TWO_ARGS and n - 1 != 2:
                errors.append(f"line {start}: ({head} ...) with {n - 1} "
                              f"arguments; the guide documents two")
            if stack:
                stack[-1][1] += 1
            continue
        if stack:
            if stack[-1][1] == 0 and kind == "atom":
                stack[-1][0] = text.lower()
                if text.lower() == "askchoice":
                    errors.append(f"line {line}: askchoice (its result is "
                                  f"not documented unambiguously; use "
                                  f"askbool)")
            stack[-1][1] += 1
        if kind == "atom" and text.startswith("@") and \
                text.lower() not in VARIABLES:
            errors.append(f"line {line}: {text} is not a pre-defined "
                          f"variable of the Installer guide")
    return errors


if __name__ == "__main__":
    errs = check(sys.argv[1])
    for e in errs:
        print(e)
    print("check_installer:", "FAIL" if errs else "ok")
    sys.exit(1 if errs else 0)
