"""NSO loader: decompress segments, build the memory image, parse MOD0/dynamic info."""
import struct
import sys
import lz4.block

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_GNU_HASH = 0x6ffffef5
DT_SYMENT, DT_RELRSZ, DT_RELR = 11, 35, 36  # packed relative relocations (newer linkers)
DT_STRSZ, DT_JMPREL, DT_RELACOUNT = 10, 23, 0x6FFFFFF9
R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027


class Nso:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        assert raw[:4] == b'NSO0', path
        flags = struct.unpack_from('<I', raw, 0xC)[0]
        self.build_id = raw[0x40:0x60].rstrip(b'\0').hex()
        segs = []
        for i in range(3):
            file_off, mem_off, dec_size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
            comp_size = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
            data = raw[file_off:file_off + comp_size]
            if flags & (1 << i):
                data = lz4.block.decompress(data, uncompressed_size=dec_size)
            segs.append((mem_off, data))
        self.bss_size = struct.unpack_from('<I', raw, 0x3C)[0]
        self.text = (segs[0][0], len(segs[0][1]))
        self.rodata = (segs[1][0], len(segs[1][1]))
        self.data = (segs[2][0], len(segs[2][1]))
        end = segs[2][0] + len(segs[2][1]) + self.bss_size
        self.size = (end + 0xFFF) & ~0xFFF
        img = bytearray(self.size)
        for off, d in segs:
            img[off:off + len(d)] = d
        self.image = img
        self._parse_mod0()

    def u32(self, off): return struct.unpack_from('<I', self.image, off)[0]
    def u64(self, off): return struct.unpack_from('<Q', self.image, off)[0]

    def _parse_mod0(self):
        mod0 = self.u32(4)
        assert self.image[mod0:mod0 + 4] == b'MOD0'
        rel = lambda i: mod0 + struct.unpack_from('<i', self.image, mod0 + 4 + i * 4)[0]
        self.dynamic, self.bss_start, self.bss_end = rel(0), rel(1), rel(2)
        self.eh_start, self.eh_end, self.module_obj = rel(3), rel(4), rel(5)
        # Some newer toolchains link the module with ELF32 dynamic/symbol metadata even though the code is AArch64.
        # DT_SYMENT says which: 16 (Elf32_Sym) or 24 (Elf64_Sym). Entries are 8 or 16 bytes respectively.
        def entries(width):
            out = []
            off = self.dynamic
            for _ in range(512):
                if width == 8:
                    tag, val = struct.unpack_from('<II', self.image, off)
                else:
                    tag, val = self.u64(off), self.u64(off + 8)
                off += width
                if tag == DT_NULL:
                    break
                out.append((tag, val))
            return out
        e64, e32 = entries(16), entries(8)
        syment64 = next((v for t, v in e64 if t == DT_SYMENT), 0)
        syment32 = next((v for t, v in e32 if t == DT_SYMENT), 0)
        if syment64 == 24:
            self.elf32 = False
        elif syment32 == 16:
            self.elf32 = True
        else:
            self.elf32 = not any(t <= 0x40 or t >= 0x6FFFFEF0 for t, _ in e64) and any(
                t <= 0x40 or t >= 0x6FFFFEF0 for t, _ in e32
            )
        dyn = {}
        needed = []
        for tag, val in e32 if self.elf32 else e64:
            if tag == DT_NEEDED:
                needed.append(val)
            dyn.setdefault(tag, val)
        self.dyn = dyn
        self.syment = 16 if self.elf32 else 24
        strtab = dyn.get(DT_STRTAB, 0)
        self.strtab = strtab
        self.needed = [self.cstr(strtab + n) for n in needed]
        # dynsym count from DT_HASH nchain, else from DT_GNU_HASH's chains (newer SDKs' sdk has only that)
        self.syms = []
        nchain = 0
        if DT_HASH in dyn:
            nchain = self.u32(dyn[DT_HASH] + 4)
        elif DT_GNU_HASH in dyn:
            nchain = self.gnu_hash_nsyms(dyn[DT_GNU_HASH])
        if nchain and DT_SYMTAB in dyn:
            st = dyn[DT_SYMTAB]
            for i in range(nchain):
                o = st + i * 16 if self.elf32 else st + i * 24
                if self.elf32:
                    name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.image, o)
                else:
                    name, info, other, shndx = struct.unpack_from('<IBBH', self.image, o)
                    value, size = struct.unpack_from('<QQ', self.image, o + 8)
                self.syms.append((self.cstr(strtab + name), info, shndx, value, size))
        self.relocs = []
        rela_size = 12 if self.elf32 else 24
        for base_tag, size_tag in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
            if base_tag in dyn:
                b, n = dyn[base_tag], dyn[size_tag] // rela_size
                for i in range(n):
                    if self.elf32:
                        r_off, r_info, r_add = struct.unpack_from('<IIi', self.image, b + i * 12)
                        self.relocs.append((r_off, r_info & 0xFF, r_info >> 8, r_add))
                    else:
                        r_off, r_info, r_add = struct.unpack_from('<QQq', self.image, b + i * 24)
                        self.relocs.append((r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))
        # A 32-bit dynamic/symbol table in this dump means the module's code is ARM32, which nxrt cannot lift.
        self.arch = 'arm32' if self.elf32 else 'aarch64'
        # DT_RELR: each listed word gets the base added; its addend is the word itself (an offset in the module)
        if DT_RELR in dyn:
            for r_off in self.relr_offsets(dyn[DT_RELR], dyn.get(DT_RELRSZ, 0)):
                self.relocs.append((r_off, R_AARCH64_RELATIVE, 0, self.u64(r_off)))

    def relr_offsets(self, off, size):
        """The words a DT_RELR table relocates: an even entry is one offset, an odd one a bitmap of the 63 words after
        the last (bit i: word i - 1)."""
        out, where = [], 0
        for i in range(size // 8):
            e = self.u64(off + i * 8)
            if not e & 1:
                out.append(e)
                where = e + 8
            else:
                bits, k = e >> 1, 0
                while bits:
                    if bits & 1:
                        out.append(where + k * 8)
                    bits >>= 1
                    k += 1
                where += 63 * 8
        return out

    def gnu_hash_nsyms(self, off):
        """The symbol count from a DT_GNU_HASH table: the end of the highest bucket's chain."""
        nbuckets, symoffset, bloom, _shift = struct.unpack_from('<IIII', self.image, off)
        buckets = off + 16 + bloom * 8
        chains = buckets + nbuckets * 4
        last = max(struct.unpack_from('<%dI' % nbuckets, self.image, buckets), default=0)
        if last < symoffset:
            return symoffset
        while not self.u32(chains + (last - symoffset) * 4) & 1:
            last += 1
        return last + 1

    def cstr(self, off):
        end = self.image.index(b'\0', off)
        return self.image[off:end].decode('utf-8', 'replace')

    def imports(self):
        return [s for s in self.syms if s[0] and s[2] == 0]

    def exports(self):
        return [s for s in self.syms if s[0] and s[2] != 0]


if __name__ == '__main__':
    from collections import Counter
    for p in sys.argv[1:]:
        n = Nso(p)
        print(f'== {p}  build {n.build_id[:16]}  image 0x{n.size:x}')
        print(f'  text 0x{n.text[0]:x}+0x{n.text[1]:x}  ro 0x{n.rodata[0]:x}+0x{n.rodata[1]:x}  '
              f'data 0x{n.data[0]:x}+0x{n.data[1]:x}  bss 0x{n.bss_size:x}')
        print(f'  needed {n.needed}  syms {len(n.syms)}  imports {len(n.imports())}  exports {len(n.exports())}')
        print(f'  relocs {dict(Counter(r[1] for r in n.relocs))}')
