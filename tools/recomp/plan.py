"""Stage `plan`: the functions' C -> files on disk.

**Stable files:** chunk file <name>_NNN.c holds exactly the functions whose entry lies in the NNN-th `span` bytes of the
module's text (64 KiB by default), and every range gets its file, empty or not. Which file a function is in depends only
on its address, so a function that changes (a patch, a lifter fix, a new function found nearby) changes its own file
and no other: the build cache (runtime/build_cache.*) reuses every other object. (The app's builder compiles the files
by index while generation goes on, so the names stay <name>_NNN.c.)

Then <name>_table.c (every function's address and pointer: the game code library's table for the dispatcher) and, for
main, main_build_id.txt (part of the library's id). Files whose text is unchanged are left alone.
"""
import bisect
import gc
import io
import multiprocessing as mp
import os
import re
from concurrent.futures import ProcessPoolExecutor

from lifter import hx

from . import emit, report


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what a patch affected."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


SPAN = 0x10000  # bytes of guest text per chunk file


def chunk_text(m, game, entries, hle, fns):
    """The text of one chunk file: the functions fns (entries, in address order) and the declarations they need."""
    body, refs = [], set()
    for a in fns:
        end, labels, tables = entries[a]
        lines, r = emit.emit_function(m, game, a, end, labels, tables, hle)
        body += lines
        refs |= r
    text = ['#include "cpu.h"\n']
    text += [f'void {emit.fname(r)}(Cpu *c);\n' for r in sorted(refs)]
    text += [f'void {emit.sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
    text += ['\n'.join(body), '\n']
    return ''.join(text)


def jobs():
    """Worker processes for write_chunks: NXRT_GEN_JOBS, else every core. 1 (or no fork(): workers would have to
    rebuild the Module) writes the chunks in this process."""
    if 'fork' not in mp.get_all_start_methods():
        return 1
    try:
        n = int(os.environ.get('NXRT_GEN_JOBS', ''))
    except ValueError:
        n = 0
    return n if n > 0 else os.cpu_count() or 1


_job = None  # (m, game, entries, hle, outdir, chunks): what forked workers inherit instead of a pickled copy


def _write_chunk(k):
    """(A worker:) chunk file k; returns what emitting it added to the module's stats."""
    m, game, entries, hle, outdir, chunks = _job
    before = m.stats.copy()
    write_if_changed(os.path.join(outdir, f'{m.name}_{k:03d}.c'), chunk_text(m, game, entries, hle, chunks[k]))
    return m.stats - before


def write_chunks(m, game, entries, hle, outdir, span=SPAN, limit=0, njobs=None):
    """Emit every function into the <name>_NNN.c file of its address range; returns (address order, chunk count).

    With more than one job the files are written by forked workers (they share the Module copy-on-write, nothing
    is pickled but chunk numbers), and the progress lines still count the files done in order: the app's builder
    compiles <name>_000.. up to the reported count while generation goes on."""
    global _job
    name = m.name
    os.makedirs(outdir, exist_ok=True)
    order = sorted(entries)
    if limit:
        order = order[:limit]
    nchunks = (m.hi - m.lo + span - 1) // span
    chunks, ci = [], 0
    for k in range(nchunks):
        cj = bisect.bisect_left(order, m.lo + (k + 1) * span, ci)
        chunks.append(order[ci:cj])
        ci = cj
    njobs = min(njobs or jobs(), nchunks)
    _job = (m, game, entries, hle, outdir, chunks)
    pool = None
    if njobs > 1:
        gc.collect()
        gc.freeze()  # (so the workers' collector doesn't touch, and copy, every page of the Module)
    try:
        if njobs > 1:
            # (not multiprocessing.Pool: a worker the system kills for memory would leave it waiting forever)
            pool = ProcessPoolExecutor(njobs, mp_context=mp.get_context('fork'))
            done = pool.map(_write_chunk, range(nchunks))
        else:
            done = map(_write_chunk, range(nchunks))
        ci = 0
        for k, stats in enumerate(done):
            if pool:
                m.stats.update(stats)  # (in this process emit counted them itself)
            ci += len(chunks[k])
            report.progress(name, ci, len(order), k + 1)
    finally:
        if pool:
            pool.shutdown(cancel_futures=True)
        if njobs > 1:
            gc.unfreeze()
        _job = None
    for fn in os.listdir(outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(name) + r'_(\d{3,})\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(outdir, fn))
    return order, nchunks


def write_table(m, order, outdir):
    """<name>_table.c: the sorted function addresses and pointers (tools/gamelib_entry.c collects them)."""
    name = m.name
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {emit.fname(a)}(Cpu *c);\n')
        f.write(f'const uint64_t {name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(emit.fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint64_t {name}_fn_count = {len(order)};\n')
        write_if_changed(os.path.join(outdir, f'{name}_table.c'), f.getvalue())


def write_build_id(m, outdir):
    """The main module's build id: the game code library's id includes it (runtime/gamelib.h gamelib_full_id, nxrt.mk)."""
    if m.name == 'main':
        write_if_changed(os.path.join(outdir, 'main_build_id.txt'), m.nso.build_id[:16].upper() + '\n')
