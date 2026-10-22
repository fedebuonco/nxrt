"""Symbolize guest addresses: sym.py <addr>... [--dis N] [--game DIR | --modules name=0xbase,...] [--exefs DIR]

The game's modules and their load addresses are read from its nx_game descriptor (runtime/*_game.c in the game
folder), so this works for any game on nxrt. The extracted NSOs come from ../extracted/exefs by default.

  python3 tools/sym.py 0x7000123456 --game games/sky
  python3 tools/sym.py 0x7000123456 --game games/sky --exefs /path/to/extracted/exefs --dis 8
  python3 tools/sym.py 0x7000123456 --modules main=0x7000000000,sdk=0x7004000000
"""
import argparse
import bisect
import glob
import os
import re
import subprocess
import sys

import capstone
from nso import Nso

MODS = []                  # [(module name, load address)], from the game's descriptor or --modules
EXEFS = '../extracted/exefs'
_cache = {}


def modules_from_descriptor(desc):
    """The .modules table of an nx_game descriptor: {{"main", 0x7000000000ull}, ...}"""
    src = open(desc, encoding='utf-8', errors='replace').read()
    i = src.find('.modules')
    if i < 0:
        return []
    j = src.find('{', i)
    if j < 0:
        return []
    depth, end = 0, len(src)
    for k in range(j, len(src)):
        depth += {'{': 1, '}': -1}.get(src[k], 0)
        if depth == 0:
            end = k + 1
            break
    block = src[j:end]
    return [(m.group(1), int(m.group(2), 16))
            for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+)', block)]


def find_descriptor(game):
    """runtime/*_game.c for --game, else the one in this folder or just above it, else the only one in games/*/"""
    if game:
        found = sorted(glob.glob(os.path.join(game, 'runtime', '*_game.c')))
        if not found:
            sys.exit(f'sym.py: no runtime/*_game.c in {game} (is that a game folder?)')
        return found[0]
    for d in ('.', '..', '../..'):
        found = sorted(glob.glob(os.path.join(d, 'runtime', '*_game.c')))
        if found:
            return found[0]
    found = sorted(glob.glob(os.path.join('games', '*', 'runtime', '*_game.c')))
    if len(found) == 1:
        return found[0]
    if len(found) > 1:
        sys.exit('sym.py: several games here: pass --game games/<id>')
    return None


def mod(name):
    if name not in _cache:
        path = os.path.join(EXEFS, name)
        if not os.path.exists(path):
            sys.exit(f'sym.py: {path} not found (pass --exefs with your extracted exefs folder)')
        n = Nso(path)
        syms = sorted((s[3], s[0]) for s in n.syms if s[2] and (s[1] & 0xF) == 2)
        _cache[name] = (n, syms, [a for a, _ in syms])
    return _cache[name]


def demangle(s):
    return subprocess.run(['c++filt'], input=s, capture_output=True, text=True).stdout.strip()


def lookup(addr):
    for name, base in MODS:
        n, syms, addrs = mod(name)
        if base <= addr < base + n.size:
            off = addr - base
            i = bisect.bisect_right(addrs, off) - 1
            if i >= 0:
                return name, base, off, syms[i][1], off - syms[i][0]
            return name, base, off, None, 0
    return None


def main(argv):
    global MODS, EXEFS
    ap = argparse.ArgumentParser(description='Symbolize guest addresses (an nxrt game recomp).')
    ap.add_argument('addrs', nargs='+', help='guest addresses, hex (0x optional)')
    ap.add_argument('--dis', type=int, default=0, help='also disassemble N instructions')
    ap.add_argument('--game', help='the game folder, e.g. games/sky (its runtime/*_game.c lists the modules)')
    ap.add_argument('--modules', help='override the module list: name=0xbase,name=0xbase,...')
    ap.add_argument('--exefs', help='the extracted exefs folder (default ../extracted/exefs)')
    a = ap.parse_args(argv)

    if a.exefs:
        EXEFS = a.exefs
    if a.modules:
        try:
            MODS = [(n, int(b, 16)) for n, b in (p.split('=') for p in a.modules.split(','))]
        except ValueError:
            sys.exit('sym.py: --modules wants name=0xbase,name=0xbase,...')
    else:
        desc = find_descriptor(a.game)
        if not desc:
            sys.exit('sym.py: could not find a game descriptor: pass --game games/<id> or --modules')
        MODS = modules_from_descriptor(desc)
        if not MODS:  # (nxgame.h: an empty .modules means main at 0x7000000000 and sdk at 0x7004000000)
            MODS = [('main', 0x7000000000), ('sdk', 0x7004000000)]
        print(f'# {os.path.dirname(os.path.dirname(desc))}: ' +
              ', '.join(f'{n}={b:#x}' for n, b in MODS), file=sys.stderr)

    for name, _ in MODS:
        if not os.path.exists(os.path.join(EXEFS, name)):
            sys.exit(f'sym.py: {os.path.join(EXEFS, name)} not found: pass --exefs with your extracted exefs folder')

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for a_ in a.addrs:
        addr = int(a_, 16)
        r = lookup(addr)
        if not r:
            print(f'{addr:x}: ?')
            continue
        name, base, off, sym, delta = r
        print(f'{addr:x}: {name}+{off:x}  {demangle(sym) if sym else "?"}+{delta:x}')
        if a.dis:
            n = mod(name)[0]
            for ins in md.disasm(bytes(n.image[off:off + 4 * a.dis]), addr):
                t = ''
                if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#'):
                    rr = lookup(int(ins.op_str[1:], 16))
                    if rr and rr[3]:
                        t = '  ; ' + demangle(rr[3])[:80]
                print(f'    {ins.address:x}: {ins.mnemonic:8} {ins.op_str}{t}')


if __name__ == '__main__':
    main(sys.argv[1:])
