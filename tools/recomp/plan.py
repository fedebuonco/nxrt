"""Stage `plan`: the functions' C -> files on disk.

**Stable files:** chunk file <name>_NNN.c holds exactly the functions whose entry lies in the NNN-th `span` bytes of the
module's text (64 KiB by default), and every range gets its file, empty or not. Which file a function is in depends only
on its address, so a function that changes (a patch, a lifter fix, a new function found nearby) changes its own file
and no other: the build cache (runtime/build_cache.*) reuses every other object. (The app's builder compiles the files
by index while generation goes on, so the names stay <name>_NNN.c.)

Then <name>_table.c (every function's address and pointer: the game code library's table for the dispatcher) and, for
main, main_build_id.txt (part of the library's id). Files whose text is unchanged are left alone.
"""
import io
import os
import re

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


def write_chunks(m, game, entries, hle, outdir, span=SPAN, limit=0):
    """Emit every function into the <name>_NNN.c file of its address range; returns (address order, chunk count)."""
    name = m.name
    os.makedirs(outdir, exist_ok=True)
    order = sorted(entries)
    if limit:
        order = order[:limit]
    nchunks = (m.hi - m.lo + span - 1) // span
    ci = 0
    for k in range(nchunks):
        body, refs, fns = [], set(), []
        hi = m.lo + (k + 1) * span
        while ci < len(order) and order[ci] < hi:
            a = order[ci]
            ci += 1
            end, labels, tables = entries[a]
            lines, r = emit.emit_function(m, game, a, end, labels, tables, hle)
            body += lines
            fns.append(a)
            refs |= r
        path = os.path.join(outdir, f'{name}_{k:03d}.c')
        text = ['#include "cpu.h"\n']
        text += [f'void {emit.fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {emit.sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        report.progress(name, ci, len(order), k + 1)
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
