"""Generate C for a whole NSO module.

Usage: gen.py <name> <nso> <base> <outdir> [--game DIR] [--hle hle_symbols.txt] [--span BYTES]

--game: the game's tools folder (games/<id>/tools): its patches.py has the game's own code changes (see load_game).
NXRT_GEN_JOBS: processes that write the chunk files (default: every core; 1 writes them in this one, see plan.jobs).

FP/SIMD/CRC32 instructions are emitted as AArch64 inline asm: the generated code is for Apple Silicon.
The stages (load, discover, cfg, lift, emit, plan, report) are in tools/recomp/ (its __init__.py lists them).
"""
import argparse
import os
import sys
import time

import nxlog
from recomp import discover, plan, report
from recomp.emit import Game
from recomp.module import Module


def load_game(game_dir):
    """The game's own code changes: `patches` (games/<id>/tools/patches.py) with PATCHES and, optionally, VARIANTS.
    From game_dir when given; else the bundled one of a frozen generator (nxgen), or none (a plain recompilation)."""
    if game_dir:
        sys.path.insert(0, os.path.abspath(game_dir))
    try:
        import patches
    except ImportError:
        if game_dir:
            raise
        return Game()
    return Game(getattr(patches, 'PATCHES', {}), getattr(patches, 'VARIANTS', {}))


def hle_functions(m, path):
    """{guest address: symbol} of the SDK functions the runtime implements itself (runtime/hle/hle_symbols.txt)."""
    hle = {}
    if path and os.path.exists(path):
        want = {l.strip() for l in open(path) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[m.base + value] = name
    return hle


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    ap.add_argument('--game')
    ap.add_argument('--span', type=lambda s: int(s, 0), default=plan.SPAN)  # bytes of guest text per chunk file
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args()
    game = load_game(args.game)
    nxlog.log.info("generating %s (%s, base %#x)", args.name, args.nso, args.base)

    t0 = time.time()
    m = Module(args.name, args.nso, args.base)                      # load
    starts = discover.boundaries(m)                                  # discover (+ cfg, lift inside)
    hle = hle_functions(m, args.hle)
    entries, rounds = discover.functions(m, discover.seed(m, starts))
    report.discovered(args.name, len(starts), len(entries), rounds, m.stats, time.time() - t0)

    order, nchunks = plan.write_chunks(m, game, entries, hle, args.outdir, args.span, args.limit)  # emit + plan
    plan.write_table(m, order, args.outdir)
    plan.write_build_id(m, args.outdir)
    report.finished(args.name, nchunks, time.time() - t0)
    nxlog.log.info("generated %s: %d chunks", args.name, nchunks)


if __name__ == '__main__':
    nxlog.setup()
    try:
        main()
    except SystemExit:
        raise
    except BaseException:
        nxlog.log.exception("generation failed")
        raise
