"""Stage `cfg`: inside one function (a piece entry..end of the text): where its branches go, its jump tables, and which
indirect jumps have to fall back to "any instruction of the function" (the all-instruction fallback, emit.py).

Jump tables are resolved by running the code that computes the target in unicorn, once per index value, from the
bounds check (cmp + b.hi / b.ls) that guards it. A table is **proven** when a second run, with every other register
holding a different value, gives the same targets: then they depend on the index and the module's own data only. A
proven table's function needs no fallback; one that isn't proven gets it (emit.py).
"""
import bisect
import re

import unicorn as uc
from unicorn import arm64_const as A

from analyze import eh_frame_functions
from lifter import bits, sext

SCRATCH_MEM = 0x7300000000
# what the registers other than the index hold during the two runs (pointers into the scratch memory)
FILLS = (SCRATCH_MEM + 0x80000, SCRATCH_MEM + 0x40000)


def emu(m):
    if m.mu is None:
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        n = m.nso
        mu.mem_map(m.base, len(n.image))
        mu.mem_write(m.base, bytes(n.image))
        mu.mem_map(SCRATCH_MEM, 0x100000)
        mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
        m.mu = mu
    return m.mu


def true_function(m, a):
    """[start, end) of the .eh_frame function containing a (the compiler's function, before splitting)."""
    if not hasattr(m, '_eh'):
        m._eh = sorted(m.base + x for x in eh_frame_functions(m.nso) if m.in_text(m.base + x))
    i = bisect.bisect_right(m._eh, a) - 1
    if i < 0:
        return m.lo, m.hi
    return m._eh[i], m._eh[i + 1] if i + 1 < len(m._eh) else m.hi


def jump_table(m, br_pc, entry, end):
    """Resolve `br xN` targets from a bounds-checked jump table, or None. Targets may lie outside the piece
    entry..end when the function was split (another function branches into its middle): anywhere in the
    compiler's function (.eh_frame) holding the br is accepted; scan() makes those targets boundaries."""
    tlo, thi = true_function(m, br_pc)
    m._vlo, m._vhi = min(entry, tlo), max(end, thi)
    for form in (_jump_table_fallthrough, _jump_table_taken):
        r = form(m, br_pc, entry, end, FILLS[0])
        if r is None:
            continue
        if form(m, br_pc, entry, end, FILLS[1]) != r:  # (the targets depend on something other than the index)
            m.stats['tables_unproven'] += 1
            continue
        return r
    return None


def _index_regs(m, start, at, idx):
    """The index register plus registers holding the same value in start..at: sources it was
    copied/masked from, and copies of those (the table lookup sometimes recomputes the index
    from another copy after the bounds check)."""
    def copy_src(w):
        if (w & 0x7F800000) == 0x12000000:  # AND (imm)
            return bits(w, 5, 5)
        if (w & 0x7FE0FFE0) == 0x2A0003E0:  # MOV (ORR with zr)
            return bits(w, 16, 5)
        if (w & 0x7F800000) in (0x53000000, 0x13000000):  # UBFM/SBFM (uxtb, uxth, sxtw...)
            return bits(w, 5, 5)
        return None
    regs = {idx}
    changed = True
    while changed:
        changed = False
        for a in range(start, at, 4):
            w = m.word(a)
            rd, src = w & 31, copy_src(w)
            if src is None or src >= 31 or rd >= 31:
                continue
            for x, y in ((rd, src), (src, rd)):
                if x in regs and y not in regs:
                    regs.add(y)
                    changed = True
    return sorted(regs)


def _set_index(mu, regs, i):
    for r in regs:
        mu.reg_write(A.UC_ARM64_REG_X0 + r if r < 29 else A.UC_ARM64_REG_X29, i)


def _emulate_table(m, br_pc, entry, end, start, idx, count, set_at, fill):
    """Run start..br for each index value (set at address set_at), collect targets."""
    mu = emu(m)
    rn = bits(m.word(br_pc), 5, 5)

    def step_to(frm, to):
        pc = frm
        for _ in range(64):
            if pc == to:
                return True
            mu.emu_start(pc, 0, count=1)
            pc = mu.reg_read(A.UC_ARM64_REG_PC)
        return False

    targets = []
    try:
        for i in range(count):
            for r in range(29):
                mu.reg_write(A.UC_ARM64_REG_X0 + r, fill)
            mu.reg_write(A.UC_ARM64_REG_SP, fill)
            a2 = m._vlo
            while a2 < start:
                w2 = m.word(a2)
                if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                    mu.emu_start(a2, 0, count=1)
                a2 += 4
            if set_at != start and not step_to(start, set_at):
                return None
            _set_index(mu, [idx], i)
            if not step_to(set_at, br_pc):
                return None
            t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                            (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
            if not (m._vlo <= t < m._vhi) or t % 4:
                return None
            targets.append(t)
    except uc.UcError:
        return None
    return sorted(set(targets))


def _jump_table_taken(m, br_pc, entry, end, fill):
    """Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br"""
    # start of the block holding the br
    s = br_pc
    while s - 4 >= entry and br_pc - (s - 4) <= 4 * 12 and m.lift(s - 4).kind in ('op', 'asm'):
        s -= 4
    for g in range(entry, end, 4):
        w = m.word(g)
        if (w & 0xFF000010) != 0x54000000 or bits(w, 0, 4) not in (9, 3):
            continue
        t = g + sext(bits(w, 5, 19), 19) * 4
        if t != s:
            continue
        cw = m.word(g - 4) if g - 4 >= entry else 0
        if (cw & 0x7F80001F) != 0x7100001F or bits(cw, 22, 1):
            continue
        limit = bits(cw, 10, 12)
        count = limit + 1 if bits(w, 0, 4) == 9 else limit
        if count <= 0 or count > 4096:
            continue
        return _emulate_table(m, br_pc, entry, end, s, bits(cw, 5, 5), count, s, fill)
    return None


def _jump_table_fallthrough(m, br_pc, entry, end, fill):
    rn = bits(m.word(br_pc), 5, 5)
    # find the guarding b.hi / b.hs and its cmp, scanning backwards
    a = br_pc - 4
    guard = cmp = None
    while a >= entry and br_pc - a <= 4 * 16:
        w = m.word(a)
        if (w & 0xFF000010) == 0x54000000 and bits(w, 0, 4) in (8, 2):
            guard = a
            break
        if m.lift(a).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap'):
            return None
        a -= 4
    if guard is None:
        return None
    a = guard - 4
    while a >= entry and guard - a <= 4 * 8:
        w = m.word(a)
        # SUBS (imm) with Rd = zr, i.e. CMP #imm
        if (w & 0x7F80001F) == 0x7100001F:
            cmp = a
            break
        if m.lift(a).kind != 'op':
            return None
        a -= 4
    if cmp is None:
        return None
    w = m.word(cmp)
    if bits(w, 22, 1):
        return None
    limit = bits(w, 10, 12)
    count = limit + 1 if bits(m.word(guard), 0, 4) == 8 else limit
    if count <= 0 or count > 4096:
        return None
    idx = bits(w, 5, 5)
    # window start: back from cmp to the first control-flow instruction
    start = cmp
    while start - 4 >= entry and cmp - (start - 4) <= 4 * 12 and m.lift(start - 4).kind in ('op', 'asm'):
        start -= 4
    idx_regs = _index_regs(m, start, cmp, idx)
    mu = emu(m)
    targets = []

    def step_to(frm, to):
        # single-step: Unicorn's `until` is unreliable mid translation-block
        pc = frm
        for _ in range(64):
            if pc == to:
                return True
            mu.emu_start(pc, 0, count=1)
            pc = mu.reg_read(A.UC_ARM64_REG_PC)
        return False

    try:
        for i in range(count):
            for r in range(29):
                mu.reg_write(A.UC_ARM64_REG_X0 + r, fill)
            mu.reg_write(A.UC_ARM64_REG_SP, fill)
            # replay address materialization (ADRP/ADR/ADD-imm) from the function entry, so table
            # bases computed before loops or branches are known
            a2 = m._vlo
            while a2 < start:
                w2 = m.word(a2)
                if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                    mu.emu_start(a2, 0, count=1)
                a2 += 4
            if not step_to(start, cmp):
                return None
            _set_index(mu, idx_regs, i)
            if not step_to(cmp, br_pc):
                return None
            t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                            (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
            if not (m._vlo <= t < m._vhi) or t % 4:
                return None
            targets.append(t)
    except uc.UcError:
        return None
    return sorted(set(targets))


def computed_target(m, br_pc, entry):
    """True if the br register comes from address arithmetic (a table) rather than a load."""
    rn = bits(m.word(br_pc), 5, 5)
    a = br_pc - 4
    while a >= entry:
        w = m.word(a)
        ins = m.lift(a)
        if ins.kind not in ('op', 'asm'):
            return False
        # Vector register numbers share encoding bits with GPRs, but do not overwrite them.
        writes_rn = (bool(re.search(rf'c->x\[{rn}\]\s*=(?!=)', ins.code)) if ins.kind == 'op'
                     else (0, rn) in ins.asm.gprs)
        if writes_rn:
            if (w & 0x1F000000) == 0x0B000000:  # ADD/SUB (shifted or extended register)
                return True
            if (w & 0x9F000000) == 0x10000000:  # ADR
                return True
            return False
        a -= 4
    return False


def jump_table_base(m, adr_pc, target):
    """A table's relative branch base is a local label, not a function entry."""
    br_pc = target - 4
    return (adr_pc < br_pc and m.in_text(br_pc) and
            (m.word(br_pc) & 0xFFFFFC1F) == 0xD61F0000 and
            bits(m.word(br_pc), 5, 5) == (m.word(adr_pc) & 31) and
            computed_target(m, br_pc, adr_pc))


def scan(m, entry, end):
    """Labels, external entries and jump tables of the function entry..end: (labels, ext, tables). tables maps a br's
    address to its sorted targets (a proven table), or to 'all' (unresolved: the all-instruction fallback)."""
    labels, ext, tables = {entry}, set(), {}
    terminated = False
    for a in range(entry, end, 4):
        ins = m.lift(a)
        if ins.kind in ('b', 'bcond'):
            t = ins.target
            if entry <= t < end:
                labels.add(t)
            elif m.in_text(t):
                ext.add(t)
        elif ins.kind == 'bl':
            if m.in_text(ins.target):
                ext.add(ins.target)
        elif ins.kind == 'br':
            jt = jump_table(m, a, entry, end)
            if jt:
                tables[a] = jt
                labels.update(t for t in jt if entry <= t < end)
                ext.update(t for t in jt if not entry <= t < end)
                m.stats['jumptables'] += 1
            elif computed_target(m, a, entry):
                # unresolved table jump: allow jumping to any instruction of the function
                tables[a] = 'all'
                m.stats['br_all_labels'] += 1
            else:
                m.stats['br_dispatch'] += 1
        terminated = ins.kind in ('b', 'br', 'ret', 'trap')
    if not terminated and end < m.hi:
        ext.add(end)
    return labels, ext, tables
