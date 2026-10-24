"""Stage `emit`: one function -> its C lines. Instructions come from the lifter (lifter.py, through the module's cache);
consecutive FP/SIMD instructions are batched into one asm block; the game's patches replace single instructions and
its variants let the runtime pick an instruction (nx_code_variant).

Branches inside the function are gotos, to other functions tail calls, anywhere else rt_dispatch. A function with a
jump table also gets the all-instruction fallback: a `switch (c->pc)` over every instruction, so a table case the
emulation missed still lands on the right instruction.
"""
import re

from lifter import Unsupported, emit_asm_batch, hx

OPTNONE_INSNS = 3000  # functions with jump tables above this many instructions are compiled unoptimized


def fname(a):
    return f'f_{a:x}'


def label(a):
    return f'L_{a:x}'


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


class Game:
    """The game's own code changes (tools/patches.py): PATCHES (guest address -> function(gen, addr) returning the C for
    that one instruction) and VARIANTS (main-module offset -> instruction words the runtime may pick instead)."""
    def __init__(self, patches=None, variants=None):
        self.patches = dict(patches or {})
        self.variants = dict(variants or {})


def render_single(m, ins, a, jump, refs):
    """C for one lifted instruction (no batching); used for patch variants."""
    k = ins.kind
    if k == 'asm':
        return [emit_asm_batch([ins.asm])]
    if k in ('op', 'trap'):
        return [ins.code]
    if k == 'b':
        return [jump(ins.target)]
    if k == 'bcond':
        return [f'if ({ins.cond}) {{ {jump(ins.target)} }}']
    if k == 'bl':
        t = ins.target
        if m.in_text(t):
            refs.add(t)
            return [f'c->x[30] = {hx(a + 4)}; {fname(t)}(c);']
        return [f'c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);']
    if k == 'blr':
        return [f'c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);']
    if k == 'br':
        return [f'c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);']
    if k == 'ret':
        return ['return;']
    raise Unsupported(k)


def emit_function(m, game, entry, end, labels, tables, hle):
    """(C lines, the functions it calls by name) for the function entry..end."""
    # A huge function with the all-instruction fallback (thousands of labels, one big switch) sends clang's
    # optimizer into the hours (main 1.0.0: f_7000a7c130, f_7001389610 - over an hour each at -O1): unoptimized
    # those compile in seconds.
    huge = bool(tables) and (end - entry) // 4 > OPTNONE_INSNS and entry not in hle
    attr = '__attribute__((optnone, noinline)) ' if huge else ''
    if huge:
        m.stats['optnone'] += 1
    out = [f'{attr}void {fname(entry)}(Cpu *CPU_RESTRICT c) {{']
    refs = set()
    if entry in hle:
        out.append(f'    {sanitize(hle[entry])}(c);')
        out.append('}')
        return out, refs
    out.append(f'    FN_ENTER({hx(entry)});')
    batch = []

    def flush():
        if batch:
            out.append('    ' + emit_asm_batch(batch))
            batch.clear()

    def jump(t):
        if entry <= t < end:
            return f'goto {label(t)};'
        if not m.in_text(t):
            return f'c->pc = {hx(t)}; __attribute__((musttail)) return rt_dispatch(c);'
        refs.add(t)
        return f'__attribute__((musttail)) return {fname(t)}(c);'

    # Functions with jump tables also get the all-instruction fallback: a table whose emulation
    # missed some cases then still reaches the right instruction instead of aborting.
    all_labels = bool(tables)
    if all_labels:
        labels = set(range(entry, end, 4))
    variants = game.variants if m.name == 'main' else {}  # (the game's variant sites are main-module offsets)
    rando_here = [a for a in range(entry, end, 4) if a - m.base in variants]
    if rando_here:
        labels = set(labels)
        for a in rando_here:
            for w in game.variants[a - m.base]:
                v = m.lifter.lift(w, a)
                if v.kind in ('b', 'bcond') and isinstance(v.target, int) and entry <= v.target < end:
                    labels.add(v.target)
    last = None
    for a in range(entry, end, 4):
        ins = m.lift(a)
        if a in labels:
            flush()
            out.append(f'{label(a)}:;')
        if a - m.base in variants:  # variant site: the game picks the instruction at run time (e.g. a seed)
            flush()
            out.append(f'    {{ extern uint32_t nx_code_variant(uint64_t); uint32_t rw = nx_code_variant({hx(a)});')
            first = True
            for w in game.variants[a - m.base]:
                v = m.lifter.lift(w, a)
                out.append(f'    {"if" if first else "else if"} (rw == {hx(w)}) {{ ' + ' '.join(render_single(m, v, a, jump, refs)) + ' }')
                first = False
            out.append('    else { ' + ' '.join(render_single(m, ins, a, jump, refs)) + ' } }')
            last = 'op'
            continue
        if a in game.patches:  # hand-written replacement for one instruction (the game's patches.py)
            flush()
            out.append('    ' + game.patches[a](m, a))
            last = 'op'
            continue
        k = ins.kind
        if k == 'asm':
            if len(batch) >= 24:
                flush()
            batch.append(ins.asm)
            try:
                emit_asm_batch(batch)
            except Unsupported:
                batch.pop()
                flush()
                batch.append(ins.asm)
            last = k
            continue
        flush()
        if k == 'op' or k == 'trap':
            out.append('    ' + ins.code)
        elif k == 'b':
            out.append('    ' + jump(ins.target))
        elif k == 'bcond':
            out.append(f'    if ({ins.cond}) {{ {jump(ins.target)} }}')
        elif k == 'bl':
            t = ins.target
            if m.in_text(t):
                refs.add(t)
                out.append(f'    c->x[30] = {hx(a + 4)}; {fname(t)}(c);')
            else:
                out.append(f'    c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);')
        elif k == 'blr':
            out.append(f'    c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);')
        elif k == 'br':
            jt = tables.get(a)
            if jt == 'all':
                out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                continue
            if jt:
                cases = ' '.join(f'case {hx(t)}: {jump(t)}' for t in jt)
                out.append(f'    switch ({ins.target}) {{ {cases} default: break; }}')
                out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                continue
            out.append(f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
        elif k == 'ret':
            out.append('    return;')
        last = k
    flush()
    if last not in ('b', 'br', 'ret', 'trap'):
        out.append('    ' + jump(end) if end < m.hi else f'    rt_udf(c, {hx(end)});')
    if all_labels:
        out.append('L_dispatch:')
        cases = ' '.join(f'case {hx(t)}: goto {label(t)};' for t in range(entry, end, 4))
        out.append(f'    switch (c->pc) {{ {cases} default: break; }}')
        out.append('    __attribute__((musttail)) return rt_dispatch(c);')
    out.append('}')
    return out, refs
