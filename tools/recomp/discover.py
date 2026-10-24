"""Stage `discover`: where the module's functions start.

Seeds: unwind data (.eh_frame), function symbols, relocated pointers into the text, `bl` targets, and code addresses
built in registers (ADR, ADRP + ADD: callbacks, vtables made at run time). Then every function is scanned (cfg.py), and
any branch or call target outside its function becomes a new start, splitting that function, until nothing changes.
"""
from analyze import eh_frame_functions
from nso import R_AARCH64_RELATIVE

from . import cfg


def sext26(w):
    v = w & 0x03FFFFFF
    return v - 0x04000000 if v & 0x02000000 else v


def boundaries(m):
    """The starts the module itself declares: unwind data, function symbols, relocated pointers."""
    n, b = m.nso, m.base
    starts = set()
    for a in eh_frame_functions(n):
        starts.add(b + a)
    for name, info, shndx, value, size in n.syms:
        if shndx and (info & 0xF) == 2:
            starts.add(b + value)
    for off, typ, sym, add in n.relocs:
        if typ == R_AARCH64_RELATIVE:
            starts.add(b + add)
    starts = sorted(a for a in starts if m.in_text(a))
    return starts


def seed(m, starts):
    """boundaries() plus the code's own references: bl targets and addresses built in registers."""
    start_set = set(starts)
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    # Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or
    # ADRP followed shortly by ADD on the same register.
    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            target = pc + imm
            # Scheduling can place many instructions between the ADR and its table branch.
            is_jt_base = cfg.jump_table_base(m, pc, target) or any(
                (m.words[i + k] & 0xFFFFFC1F) == 0xD61F0000 for k in range(1, 8) if i + k < len(m.words))
            if not is_jt_base:
                if m.in_text(target):
                    start_set.add(target)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t):
                    start_set.add(t)
    return start_set


def functions(m, start_set):
    """Partition the text into functions; any branch/call target outside its function becomes a new boundary
    (splitting the containing function) until a fixpoint is reached. Returns ({entry: (end, labels, tables)},
    rounds)."""
    scanned = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = cfg.scan(m, a, end)
            new |= scanned[key][1] - start_set
        if not new:
            break
        start_set |= new
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, labels, tables)
    return entries, rounds
