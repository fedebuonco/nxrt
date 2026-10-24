"""Stage `load`: one game module (an NSO) loaded at its guest address, its code words and the lifted-instruction cache.

The Module is also what the game's patches see (patches.py: PATCHES[addr](gen, addr) reads gen.base and gen.nso).
"""
import struct
from collections import Counter

from lifter import Insn, Lifter, Unsupported, hx
from nso import Nso


class Module:
    def __init__(self, name, path, base):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        self.words = struct.unpack(f'<{n.text[1] // 4}I', bytes(n.image[n.text[0]:n.text[0] + n.text[1]]))
        self.lifter = Lifter(base)
        self.cache = {}
        self.stats = Counter()
        self.mu = None  # (cfg.py: the unicorn instance that emulates jump tables, made on first use)

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is None:
            try:
                ins = self.lifter.lift(self.word(a), a)
            except Unsupported as e:
                self.stats['unsupported'] += 1
                ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
            self.cache[a] = ins
        return ins
