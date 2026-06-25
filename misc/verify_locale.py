#!/usr/bin/env python3
"""Verify the integrity of a ClassiCube locale file.

Usage:
    python3 misc/verify_locale.py [locale/es.txt ...]

With no arguments, every locale/*.txt file is checked.

For each locale file it checks that:
  1. Every non-comment, non-blank line is a valid key=value pair.
  2. There are no duplicate keys (an error if the values differ).
  3. Every character in keys and values exists in the game's CP437 charset -
     anything else would be displayed as '?' in game.
  4. Every key matches a real English string literal somewhere in src/, which
     catches dead keys and typos that would never actually be looked up.

Returns a non-zero exit code if any locale file has errors.
"""
import os, re, sys, glob

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC  = os.path.join(ROOT, "src")


def parse_unichar_array(text, name):
    m = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))] if m else []


def build_cp437():
    """Build the set of Unicode codepoints representable in the game's charset,
    read directly from the conversion tables in String.c."""
    with open(os.path.join(SRC, "String.c"), encoding="utf-8") as f:
        s = f.read()
    cp = set(range(0x20, 0x7F))  # printable ASCII
    cp |= set(parse_unichar_array(s, "controlChars"))
    cp |= set(parse_unichar_array(s, "extendedChars"))
    cp.discard(0)  # NUL placeholder in the control char table
    return cp


def index_literals():
    """All quoted string literals across src/*.c (the translatable keys)."""
    literals = set()
    for path in glob.glob(os.path.join(SRC, "*.c")):
        with open(path, encoding="utf-8", errors="replace") as f:
            literals.update(re.findall(r'"((?:[^"\\]|\\.)*)"', f.read()))
    return literals


def verify(path, cp437, literals):
    errors, warnings = [], []
    keys = {}
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if "=" not in line:
                errors.append("line %d: no '=' separator: %r" % (n, line))
                continue
            key, value = line.split("=", 1)
            if key in keys:
                prev_n, prev_v = keys[key]
                if prev_v == value:
                    warnings.append("line %d: duplicate key %r (same value as line %d)" % (n, key, prev_n))
                else:
                    errors.append("line %d: duplicate key %r differs from line %d (%r vs %r)"
                                  % (n, key, prev_n, prev_v, value))
            keys[key] = (n, value)
            for field, label in ((key, "key"), (value, "value")):
                for ch in field:
                    if ord(ch) not in cp437:
                        errors.append("line %d: %s char %r (U+%04X) not in CP437: %r"
                                      % (n, label, ch, ord(ch), field))
            if key not in literals:
                errors.append("line %d: key %r not found as a string literal in src/" % (n, key))

    print("%s: %d entries" % (os.path.relpath(path, ROOT), len(keys)))
    for w in warnings:
        print("  WARN: " + w)
    for e in errors:
        print("  ERROR: " + e)
    return len(errors) == 0


def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, "locale", "*.txt")))
    if not files:
        print("No locale files found")
        return 1
    cp437 = build_cp437()
    literals = index_literals()
    print("CP437 codepoints: %d | source literals: %d\n" % (len(cp437), len(literals)))
    ok = all(verify(f, cp437, literals) for f in files)
    print("\nRESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
