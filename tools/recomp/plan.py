"""Stage `plan`: the functions' C -> files on disk.

Functions go into chunk files in address order, up to a count and a size each (even compile times); a big function
gets a file of its own, so those compile in parallel. Then <name>_table.c (every function's address and pointer: the
game code library's table for the dispatcher) and, for main, main_build_id.txt (part of the library's id).
Files whose text is unchanged are left alone, so a rebuild recompiles only what changed.
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


def write_chunks(m, game, entries, hle, outdir, chunk=1500, chunk_bytes=1500000, big_fn=100000, limit=0):
    """Emit every function into <name>_NNN.c files; returns (address order, chunk count)."""
    name = m.name
    os.makedirs(outdir, exist_ok=True)
    order = sorted(entries)
    if limit:
        order = order[:limit]
    nchunks = 0
    ci = 0
    pending = None  # (address, lines, refs) of a big function that starts the next file
    while ci < len(order) or pending:
        body, refs, size, fns = [], set(), 0, []
        while (pending or ci < len(order)) and len(fns) < chunk and size < chunk_bytes:
            if pending:
                a, lines, r = pending
                pending = None
            else:
                a = order[ci]
                ci += 1
                end, labels, tables = entries[a]
                lines, r = emit.emit_function(m, game, a, end, labels, tables, hle)
            n = sum(len(l) + 1 for l in lines)
            if n > big_fn and fns:  # (a big function gets a file of its own: they compile in parallel)
                pending = (a, lines, r)
                break
            body += lines
            size += n
            fns.append(a)
            refs |= r
            if n > big_fn:
                break
        path = os.path.join(outdir, f'{name}_{nchunks:03d}.c')
        text = ['#include "cpu.h"\n']
        text += [f'void {emit.fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {emit.sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        report.progress(name, ci, len(order), nchunks)
    for fn in os.listdir(outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(name) + r'_(\d{3})\.c', fn)
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
