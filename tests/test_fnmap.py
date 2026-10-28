#!/usr/bin/env python3
"""Function matching (tools/fnmap.py): unique exact fingerprints map; duplicates and changes do not."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fnmap  # noqa: E402


def main():
    a = [(0x100, b"AAAA"), (0x200, b"BBBBB"), (0x300, b"CCCCCC")]
    b = [(0x400, b"BBBBB"), (0x500, b"AAAA"), (0x600, b"CCCCCC"), (0x700, b"NEW")]
    m = fnmap.match_functions(a, b)
    assert m == {0x100: 0x500, 0x200: 0x400, 0x300: 0x600}, m

    # A duplicate fingerprint in either build is ambiguous and is not mapped.
    dup = [(0x10, b"same"), (0x20, b"same")]
    one = [(0x30, b"same")]
    assert fnmap.match_functions(dup, one) == {}
    assert fnmap.match_functions(one, dup) == {}

    # Length is part of the fingerprint: a prefix does not match a longer function.
    assert fnmap.fingerprint(b"abc") != fnmap.fingerprint(b"abcabc")

    # Empty and one-byte functions still have distinct hashes.
    assert fnmap.fingerprint(b"") != fnmap.fingerprint(b"\0")
    assert fnmap.match_functions([], []) == {}

    print("test_fnmap: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
