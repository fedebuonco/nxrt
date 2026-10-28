"""Function matching across two builds of a module: exact, length-tagged fingerprints, unique matches only.

This is the safe subset of nxnative's `nso_fnmap`/`MATCH_CUTS`: a function whose bytes are identical in both builds is
matched; duplicates and changed functions are left out, never guessed. The NSO CLI is `tools/nso_fnmap.py`.
"""
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = (1 << 64) - 1


def fingerprint(data):
    """FNV-1a 64 of the function's length and bytes (the length keeps a prefix from colliding with a shorter function)."""
    h = FNV_OFFSET
    n = len(data)
    for b in (n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, (n >> 24) & 0xFF):
        h ^= b
        h = (h * FNV_PRIME) & MASK
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK
    return h


def match_functions(a, b):
    """a, b: [(address, bytes)]. Returns {address_a: address_b} for fingerprints unique in both builds."""
    def by_hash(items):
        out = {}
        for addr, data in items:
            out.setdefault(fingerprint(data), []).append(addr)
        return out
    ha, hb = by_hash(a), by_hash(b)
    out = {}
    for h, aa in ha.items():
        bb = hb.get(h)
        if bb is not None and len(aa) == 1 and len(bb) == 1:
            out[aa[0]] = bb[0]
    return out
