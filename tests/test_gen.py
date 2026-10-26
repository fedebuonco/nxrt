"""tools/recomp with forked workers (workers.py: discover's scans, plan's chunk files) against the same generation in
one process: every file byte for byte, the progress lines the app's builder reads, and the module's stats.
The module is synthetic: a few hundred small functions of real AArch64 code (calls, branches, FP, a computed jump, an
HLE function, a patched instruction), cut into many chunks."""
import contextlib
import io
import os
import random
import re
import struct
import sys
import tempfile
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))

from lifter import Lifter
from recomp import discover, plan, workers
from recomp.emit import Game
from recomp.module import Module

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}")


BASE = 0x7100000000
TEXT = 0x4000
RET, NOP, ADD, MOV, FADD, FMUL, BR = 0xD65F03C0, 0xD503201F, 0x91000400, 0xAA0203E1, 0x1E212800, 0x1E210800, 0xD61F0020
ADDREG = 0x8B020021  # add x1, x1, x2: the br target computed (cfg.computed_target)


def code(r):
    """(words, function starts): functions of 2-40 instructions, each ending in ret."""
    words, starts = [], []
    while len(words) < TEXT // 4 - 48:
        starts.append(len(words))
        n = r.randrange(2, 40)
        for i in range(n - 1):
            k = r.random()
            if k < 0.15 and starts[:-1]:  # bl to an earlier function
                words.append(0x94000000 | ((r.choice(starts[:-1]) - len(words)) & 0x3FFFFFF))
            elif k < 0.25 and i + 2 < n:  # b.ne forward, inside the function
                words.append(0x54000001 | (r.randrange(1, n - 1 - i) << 5))
            elif k < 0.45:
                words.append(r.choice((FADD, FMUL)))
            else:
                words.append(r.choice((ADD, MOV, NOP)))
        words.append(RET)
    starts.append(len(words))
    words += [ADDREG, BR] + [NOP] * (TEXT // 4 - len(words) - 2)  # a computed jump: the all-instruction fallback
    return words, starts


class FakeNso:
    syms, relocs, build_id = [], [], "00" * 32


def module(words, starts):
    m = Module.__new__(Module)  # (Module.__init__ reads an NSO file: same fields, from words)
    m.name, m.nso, m.base = "main", FakeNso(), BASE
    m.lo, m.hi = BASE, BASE + 4 * len(words)
    m.words = tuple(words)
    m.lifter, m.cache, m.stats, m.mu = Lifter(BASE), {}, Counter(), None
    m._eh = [BASE + 4 * s for s in starts]  # (cfg.true_function: the unwind data's functions)
    return m


def generate(njobs, outdir):
    os.environ["NXRT_GEN_JOBS"] = str(njobs)
    r = random.Random(7)
    words, starts = code(r)
    m = module(words, starts)
    entries, _ = discover.functions(m, discover.seed(m, [BASE + 4 * s for s in starts]))
    hle = {BASE + 4 * starts[3]: "nn::os::Foo(int)"}
    patched = BASE + 4 * starts[5] + 4
    game = Game({patched: lambda gen, a: f"/* patched {a:#x} */"})
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        order, nchunks = plan.write_chunks(m, game, entries, hle, outdir, span=0x100)
    plan.write_table(m, order, outdir)
    files = {fn: open(os.path.join(outdir, fn), "rb").read() for fn in sorted(os.listdir(outdir))}
    return files, err.getvalue(), m.stats, nchunks, len(order)


with tempfile.TemporaryDirectory() as d:
    serial, slog, sstats, nchunks, nfns = generate(1, os.path.join(d, "serial"))
    par, plog, pstats, _, _ = generate(4, os.path.join(d, "parallel"))

    check(nchunks == TEXT // 0x100, f"{nchunks} chunks")
    check(nfns > 100, f"{nfns} functions")
    check(len(serial) == nchunks + 1, f"{len(serial)} files")
    check(sorted(serial) == sorted(par), "the same file names")
    for fn in serial:
        check(serial[fn] == par.get(fn), f"{fn} byte for byte")
    check(any(b"hle_nn__os__Foo_int_" in t for t in serial.values()), "the HLE function's call")
    check(any(b"/* patched" in t for t in serial.values()), "the patch")
    check(any(b"L_dispatch" in t for t in serial.values()), "the all-instruction fallback")
    check(slog == plog, "the same progress lines")
    prog = [tuple(map(int, mm)) for mm in re.findall(r"^main: progress (\d+)/(\d+) files (\d+)$", plog, re.M)]
    check(len(prog) == nchunks, f"{len(prog)} progress lines")
    check(all(t == nfns for _, t, _ in prog), "progress totals")
    check([f for _, _, f in prog] == list(range(1, nchunks + 1)), "files counted in order")
    check(all(a[0] <= b[0] for a, b in zip(prog, prog[1:])) and prog[-1][0] == nfns, "functions done grow to all")
    check(sstats == pstats, f"the same stats: {dict(sstats)} / {dict(pstats)}")

    # an earlier run's files: unchanged ones keep their time, a smaller span's extra chunks go
    out = os.path.join(d, "parallel")
    old = os.path.join(out, "main_000.c")
    os.utime(old, (1, 1))
    open(os.path.join(out, f"main_{nchunks + 5:03d}.c"), "w").close()
    generate(4, out)
    check(os.stat(old).st_mtime == 1, "an unchanged file left alone")
    check(not os.path.exists(os.path.join(out, f"main_{nchunks + 5:03d}.c")), "an extra chunk removed")

    os.environ["NXRT_GEN_JOBS"] = "3"
    check(workers.jobs() == 3, "NXRT_GEN_JOBS")
    os.environ["NXRT_GEN_JOBS"] = "x"
    check(workers.jobs() == (os.cpu_count() or 1), "NXRT_GEN_JOBS unset or bad: every core")

print(f"{checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
