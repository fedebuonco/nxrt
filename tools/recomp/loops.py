"""Spin-wait detection (stage `cfg`/`emit` companion).

A guest loop that polls memory with no SVC burns a host core: the recompiled C runs the loop as written, and the kernel
never sees it. This module finds a backward branch whose loop is provably self-contained and lets emit.py put a gate on
that back-edge. The rules are nxnative's (dm/tools/patch_nso.py, "Spin waits"), reduced to what the lifter gives us:

  - every path back to the head runs only loads, register/flag arithmetic, direct branches and barriers;
  - no store, no call, no SVC, no system register, no exclusive/atomic read-modify-write;
  - nothing live at the head is written on the way (a counter, cursor or delay written in the loop makes it work);
  - the loop loads at least once (a loop of branches alone is not a wait).

The classifier is conservative: a loop it rejects is not gated, which costs only the optimisation. It is used only when
`NXRT_SPIN_GATE=1` is set for generation, so a false positive cannot change an ordinary build.
"""
from __future__ import annotations

# Instructions that must not appear in a wait loop. `st*` covers the stores; the atomics write memory as well.
_BAD_PREFIX = ("st", "cas", "swp", "ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin",
               "ldxr", "ldaxr", "ldxp", "ldaxp")
_BAD = {"bl", "blr", "br", "ret", "svc", "hvc", "smc", "mrs", "msr", "sys", "sysl", "dc", "ic", "at", "tlbi", "eret",
        "drps", "wfe", "wfi", "sev", "sevl"}


def _normalize(ins, rid):
    """x0 and w0 are one register for liveness; sp/lr stay distinct."""
    n = ins.reg_name(rid)
    if not n:
        return None
    if n in ("sp", "wsp"):
        return "sp"
    if n in ("lr",):
        return "x30"
    if n[:1] in ("x", "w") and n[1:].isdigit():
        return "x" + n[1:]
    return n if n.startswith(("x", "v", "q", "d", "s", "h", "b", "z", "p")) else None


def classify(instrs, header, site):
    """(safe, why) for the loop body instrs[header..site] (inclusive), where site branches back to header.

    instrs: dicts with addr, mnem, reads (set of names), writes (set of names). The caller has already checked that
    site is a backward branch to header.
    """
    body = instrs[header:site + 1]
    if not body:
        return False, "empty"
    has_load = False
    written, live_in = set(), set()
    for ins in body:
        mnem = ins["mnem"]
        if mnem in _BAD or mnem.startswith(_BAD_PREFIX):
            return False, f"{mnem} at {ins['addr']:#x}"
        if mnem.startswith("ld"):
            has_load = True  # (ldr/ldp/ldur/ld1... a wait polls memory)
        for r in ins["reads"]:
            if r not in written:
                live_in.add(r)
        written |= ins["writes"]
    if not has_load:
        return False, "no load"
    if live_in & written:
        return False, f"writes live-in register {sorted(live_in & written)[0]}"
    return True, ""


def _capstone():
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    return md


def _target(ins):
    from capstone.arm64 import ARM64_OP_IMM
    for op in ins.operands:
        if op.type == ARM64_OP_IMM:
            return op.imm
    return None


def safe_backedges(m, entry, end):
    """{back-edge address: header address} of the safe loops in one function. Empty when capstone is unavailable."""
    import struct
    try:
        md = _capstone()
    except Exception:
        return {}
    n = (end - entry) // 4
    if n <= 0:
        return {}
    data = struct.pack(f"<{n}I", *[m.word(a) for a in range(entry, end, 4)])
    insns, by_addr = [], {}
    for ci in md.disasm(data, entry):
        reads = {_normalize(ci, r) for r in ci.regs_access()[0]}
        writes = {_normalize(ci, r) for r in ci.regs_access()[1]}
        rec = {"addr": ci.address, "mnem": ci.mnemonic, "reads": {r for r in reads if r},
               "writes": {r for r in writes if r}, "target": _target(ci)}
        by_addr[ci.address] = len(insns)
        insns.append(rec)
    out = {}
    for i, ins in enumerate(insns):
        t = ins["target"]
        if t is None or t >= ins["addr"] or t not in by_addr:
            continue
        h = by_addr[t]
        ok, _ = classify(insns, h, i)
        if ok:
            out[ins["addr"]] = t
    return out
