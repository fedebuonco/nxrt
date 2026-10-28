#!/usr/bin/env python3
"""Spin-wait classifier (tools/recomp/loops.py): safe polls accepted, work loops and side effects rejected."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from recomp import loops  # noqa: E402


def ins(addr, mnem, reads=(), writes=(), target=None):
    return {"addr": addr, "mnem": mnem, "reads": set(reads), "writes": set(writes), "target": target}


def safe(instrs, header, site):
    ok, why = loops.classify(instrs, header, site)
    return ok, why


def main():
    # ldr w0,[x1]; cbnz w0, -4: a poll. x1 is live-in and untouched; w0 is written before each read.
    poll = [ins(0, "ldr", {"x1"}, {"x0"}), ins(4, "cbnz", {"x0"}, set(), 0)]
    ok, why = safe(poll, 0, 1)
    assert ok, why

    # add w0,w0,#1; b 0: a counter, so the head's w0 is live and written.
    counter = [ins(0, "ldr", {"x2"}, {"x1"}), ins(4, "add", {"x0"}, {"x0"}), ins(8, "b", set(), set(), 0)]
    ok, why = safe(counter, 0, 2)
    assert not ok and "live-in" in why, why

    # str w0,[x1]; b 0: a store makes it not a wait.
    store = [ins(0, "str", {"x0", "x1"}, set()), ins(4, "b", set(), set(), 0)]
    ok, why = safe(store, 0, 1)
    assert not ok and "str" in why, why

    # bl: a call can change anything.
    call = [ins(0, "bl", set(), {"x30"}, 0x100), ins(4, "b", set(), set(), 0)]
    ok, why = safe(call, 0, 1)
    assert not ok and "bl" in why, why

    # b 0 alone: no load, so not a wait.
    empty = [ins(0, "b", set(), set(), 0)]
    ok, why = safe(empty, 0, 0)
    assert not ok and why == "no load", why

    # An atomic read-modify-write writes memory.
    atomic = [ins(0, "ldadd", {"x0", "x1"}, set()), ins(4, "b", set(), set(), 0)]
    ok, why = safe(atomic, 0, 1)
    assert not ok and "ldadd" in why, why

    # A load and a barrier are allowed.
    barrier = [ins(0, "ldar", {"x1"}, {"x0"}), ins(4, "dmb", set(), set()), ins(8, "b", set(), set(), 0)]
    ok, why = safe(barrier, 0, 2)
    assert ok, why

    # A dead store after the last read is still a store.
    dead = [ins(0, "ldr", {"x1"}, {"x0"}), ins(4, "str", {"x0", "x1"}, set()), ins(8, "b", set(), set(), 0)]
    ok, why = safe(dead, 0, 2)
    assert not ok, why

    # The capstone path: a real ldr/cbnz loop is found; a counter loop is rejected.
    class M:
        def __init__(self, words, base):
            self.words, self.lo, self.base = words, base, base
            self.hi = base + 4 * len(words)
        def word(self, a):
            return self.words[(a - self.lo) // 4]

    base = 0x100000
    poll = M([0xB9400020, 0x35FFFFE0, 0xD65F03C0], base)  # ldr w0,[x1]; cbnz w0,-4; ret
    got = loops.safe_backedges(poll, base, poll.hi)
    assert got == {base + 4: base}, got

    counter = M([0xB9400041, 0x11000400, 0x35FFFFC0, 0xD65F03C0], base)  # load, add w0, cbnz -8, ret
    got = loops.safe_backedges(counter, base, counter.hi)
    assert got == {}, got

    print("test_loops: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
