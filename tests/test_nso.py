"""tools/nso.py against a synthetic NSO built by this test: header, segments (plain and LZ4), MOD0, the dynamic
table, symbols/imports/exports and relocations. The recompiler's loader is the front door for every game's code."""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import lz4.block

from nso import Nso

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}")


MOD0 = 0x80
DYN = 0x100
HASH = 0x200
STRTAB = 0x240
SYMTAB = 0x280
RELA = 0x300
RELR = 0x340
TEXT = 0x400
RO = 0x400
DATA = 0x500
DATA_LEN = 0x100
BSS = 0x80


def make_text(gnu=False):
    t = bytearray(TEXT)
    struct.pack_into("<I", t, 0, 0x12345678)
    struct.pack_into("<I", t, 4, MOD0)  # the MOD0 offset, as the loader reads it
    t[MOD0:MOD0 + 4] = b"MOD0"
    # relative offsets from MOD0: dynamic, bss start/end, eh start/end, module object
    struct.pack_into("<6i", t, MOD0 + 4, DYN - MOD0, 0, 0, 0, 0, 0)
    dyn = [
        (0x6FFFFEF5 if gnu else 4, HASH),  # DT_GNU_HASH (newer SDKs' sdk has only that) or DT_HASH
        (5, STRTAB),    # DT_STRTAB
        (6, SYMTAB),    # DT_SYMTAB
        (10, 0x10),     # DT_STRSZ
        (1, 1),         # DT_NEEDED ("libnx")
        (7, RELA),      # DT_RELA
        (8, 24),        # DT_RELASZ
    ] + ([
        (36, RELR),     # DT_RELR (packed relative relocations, newer linkers)
        (35, 16),       # DT_RELRSZ
    ] if gnu else []) + [
        (0, 0),         # DT_NULL
    ]
    off = DYN
    for tag, val in dyn:
        struct.pack_into("<QQ", t, off, tag, val)
        off += 16
    # DT_HASH: nbucket, nchain (the loader takes nchain as the symbol count), then buckets and chains
    if gnu:
        # DT_GNU_HASH: nbuckets 1, symoffset 1, one bloom word, shift 0; bloom; bucket 1; chains for symbols 1, 2 (the
        # last one ends the chain: bit 0) - so 3 symbols
        struct.pack_into("<IIII", t, HASH, 1, 1, 1, 0)
        struct.pack_into("<QI", t, HASH + 16, 0, 1)
        struct.pack_into("<II", t, HASH + 28, 0x1234 & ~1, 0x5678 | 1)
    else:
        struct.pack_into("<II", t, HASH, 1, 3)
    # strings: "" / "libnx" / "foo" / "bar"
    strings = b"\0libnx\0foo\0bar\0"
    t[STRTAB:STRTAB + len(strings)] = strings
    # symbols: null, an import (shndx 0), an export (shndx 1)
    syms = [
        (0, 0, 0, 0, 0, 0),
        (1, 0x12, 0, 0, 0, 0),
        (7, 0x12, 0, 1, 0x10, 8),
    ]
    for i, (name, info, other, shndx, value, size) in enumerate(syms):
        struct.pack_into("<IBBHQQ", t, SYMTAB + i * 24, name, info, other, shndx, value, size)
    # one R_AARCH64_JUMP_SLOT relocation for the import
    struct.pack_into("<QQq", t, RELA, 0x20, (1 << 32) | 1026, 0)
    if gnu:
        # DT_RELR: 0x3A0 itself, then a bitmap of the words after it - bits 1 and 3: 0x3A8 and 0x3B8
        struct.pack_into("<QQ", t, RELR, 0x3A0, 0b1011)
        for off, val in ((0x3A0, 0x111), (0x3A8, 0x222), (0x3B8, 0x333)):
            struct.pack_into("<Q", t, off, val)
    return t


def build(path, compress=False, gnu=False):
    text = make_text(gnu)
    ro = bytes((i * 3) & 0xFF for i in range(0x100))
    data = bytes((i * 5 + 1) & 0xFF for i in range(DATA_LEN))
    if compress:
        stored_text = lz4.block.compress(bytes(text), store_size=False)
        flags = 1
    else:
        stored_text = bytes(text)
        flags = 0
    hdr = bytearray(0x100)
    hdr[0:4] = b"NSO0"
    struct.pack_into("<I", hdr, 4, 0)
    struct.pack_into("<I", hdr, 0xC, flags)
    struct.pack_into("<I", hdr, 0x3C, BSS)
    hdr[0x40:0x60] = bytes(range(32))
    body = bytearray()
    segs = [(0, text, stored_text), (RO, ro, ro), (DATA, data, data)]
    off = 0x100
    for mem, plain, stored in segs:
        struct.pack_into("<III", hdr, 0x10 + segs.index((mem, plain, stored)) * 0x10, off, mem, len(plain))
        body += stored
        off += len(stored)
    for i, (_, plain, stored) in enumerate(segs):
        struct.pack_into("<I", hdr, 0x60 + i * 4, len(stored))
    with open(path, "wb") as f:
        f.write(hdr + body)


def expected(n, relr=False):
    check(n.text == (0, TEXT), "text segment")
    check(n.rodata == (RO, 0x100), "rodata segment")
    check(n.data == (DATA, DATA_LEN), "data segment")
    check(n.bss_size == BSS, "bss")
    check(n.size == 0x1000, "image size")
    check(n.build_id == bytes(range(32)).hex(), "build id")
    check(n.needed == ["libnx"], "DT_NEEDED")
    check(len(n.syms) == 3, "symbol count")
    check([s[0] for s in n.imports()] == ["libnx"], "imports")
    check([s[0] for s in n.exports()] == ["foo"], "exports")
    packed = [(0x3A0, 1027, 0, 0x111), (0x3A8, 1027, 0, 0x222), (0x3B8, 1027, 0, 0x333)] if relr else []
    check(n.relocs == [(0x20, 1026, 1, 0)] + packed, "relocations (DT_RELR decoded)" if relr else "relocations")
    check(n.dyn.get(4, n.dyn.get(0x6FFFFEF5)) == HASH and n.dyn[5] == STRTAB and n.dyn[6] == SYMTAB, "dynamic table")
    check(bytes(n.image[0x300:0x318]) == struct.pack("<QQq", 0x20, (1 << 32) | 1026, 0), "relocation bytes")
    check(n.image[0:4] == bytes([0x78, 0x56, 0x34, 0x12]), "text bytes")


def main():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "main")
        build(p, compress=False)
        expected(Nso(p))
        build(p, compress=True)
        expected(Nso(p))
        build(p, compress=True, gnu=True)  # newer linkers: DT_GNU_HASH alone, and DT_RELR
        expected(Nso(p), relr=True)
        # A file that is not an NSO is refused (assert), not parsed into nonsense.
        with open(p, "wb") as f:
            f.write(b"NOTANSO")
        try:
            Nso(p)
            check(False, "a non-NSO file must be refused")
        except AssertionError:
            check(True, "a non-NSO file is refused")
    print(f"{checks} checks passed" if not failures else f"{failures} of {checks} checks failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
