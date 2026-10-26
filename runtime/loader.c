// NSO loading, relocation, and symbol resolution.
#include <stdlib.h>
#include <string.h>

#include "lz4_block.h"
#include "rt.h"
#include "nxgame.h"

Module g_modules[8];
int g_nmodules;

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
       DT_RELRSZ = 35, DT_RELR = 36, DT_GNU_HASH = 0x6ffffef5 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;

// The symbol count from DT_GNU_HASH, for a module linked without DT_HASH (newer SDKs' sdk): the last symbol is the
// end of the chain of the highest bucket (nbuckets, symoffset, bloom words, bloom shift; bloom; buckets; chains).
// 0 when the table doesn't fit in the module.
static uint64_t gnu_hash_nsyms(uint64_t base, uint64_t size, uint64_t off) {
    if (off > size || size - off < 16) return 0;
    const uint32_t *h = (const uint32_t *)(uintptr_t)(base + off);
    uint64_t nbuckets = h[0], symoffset = h[1], bloom = h[2];
    uint64_t buckets_off = off + 16 + bloom * 8, chains_off = buckets_off + nbuckets * 4;
    if (bloom > size || nbuckets > size || chains_off > size) return 0;
    const uint32_t *buckets = (const uint32_t *)(uintptr_t)(base + buckets_off);
    uint64_t last = 0;
    for (uint64_t i = 0; i < nbuckets; i++)
        if (buckets[i] > last) last = buckets[i];
    if (last < symoffset) return symoffset;
    for (;; last++) {
        uint64_t c = chains_off + (last - symoffset) * 4;
        if (c + 4 > size) return 0;
        if (*(const uint32_t *)(uintptr_t)(base + c) & 1) return last + 1;
    }
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *p = malloc((size_t)n ? (size_t)n : 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    if (n && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

static void load_nso(Module *m, const char *path, uint64_t base) {
    size_t len;
    uint8_t *raw = read_file(path, &len);
    if (!raw) FATAL("cannot read %s\n", path);
    if (len < 0x100 || memcmp(raw, "NSO0", 4)) FATAL("%s is not an NSO\n", path);
    uint32_t flags = *(uint32_t *)(raw + 0xC);
    uint32_t seg[3][3];  // file_off, mem_off, size
    for (int i = 0; i < 3; i++) memcpy(seg[i], raw + 0x10 + i * 0x10, 12);
    uint32_t bss = *(uint32_t *)(raw + 0x3C);
    uint64_t size = ((uint64_t)seg[2][1] + seg[2][2] + bss + 0x3FFF) & ~0x3FFFull;
    if (size < 0x1000 || size > (1ull << 32))
        FATAL("%s: implausible module size %#llx\n", path, (unsigned long long)size);
    plat_commit(base, size);  // inside the guest window: zero-filled, read/write
    for (int i = 0; i < 3; i++) {
        uint32_t csize = *(uint32_t *)(raw + 0x60 + i * 4);
        uint64_t want = (flags & (1 << i)) ? csize : seg[i][2];
        if ((uint64_t)seg[i][0] + want > len) FATAL("%s: segment %d is past the end of the file\n", path, i);
        if ((uint64_t)seg[i][1] + seg[i][2] > size) FATAL("%s: segment %d is past the end of the module\n", path, i);
        uint8_t *dst = (uint8_t *)base + seg[i][1];
        if (flags & (1 << i)) {
            size_t got = rt_lz4_decompress(raw + seg[i][0], csize, dst, seg[i][2]);
            if (got != seg[i][2]) FATAL("lz4 %s seg %d: %zu != %u\n", path, i, got, seg[i][2]);
        } else {
            memcpy(dst, raw + seg[i][0], seg[i][2]);
        }
    }
    free(raw);
    m->base = base;
    m->size = size;
    m->text_off = seg[0][1]; m->text_size = seg[0][2];
    m->ro_off = seg[1][1];   m->ro_size = seg[1][2];
    m->data_off = seg[2][1];
    m->data_size = (uint64_t)seg[2][2] + bss;

    uint32_t mod0 = *(uint32_t *)(uintptr_t)(base + 4);
    if (mod0 > size - 8 || memcmp((const void *)(uintptr_t)(base + mod0), "MOD0", 4)) FATAL("%s: bad MOD0\n", path);
    uint64_t mod = base + mod0;
    m->dynamic = mod + *(int32_t *)(uintptr_t)(mod + 4);
    if (m->dynamic < base || m->dynamic + 16 > base + size) FATAL("%s: bad dynamic table\n", path);
#define NEED(off, bytes)                                                                                               \
    do {                                                                                                               \
        if ((off) > size || (bytes) > size - (off)) FATAL("%s: dynamic entry past the module\n", path);                \
    } while (0)
    uint64_t gnu_hash = 0;
    for (uint64_t *d = (uint64_t *)(uintptr_t)m->dynamic;; d += 2) {
        if ((uintptr_t)d + 16 > (uintptr_t)(base + size)) FATAL("%s: dynamic table runs past the module\n", path);
        if (d[0] == DT_NULL) break;
        switch (d[0]) {
        case DT_SYMTAB:
            NEED(d[1], sizeof(Elf64Sym));
            m->symtab = base + d[1];
            break;
        case DT_STRTAB:
            NEED(d[1], 1);
            m->strtab = base + d[1];
            break;
        case DT_HASH:
            NEED(d[1], 8);
            m->nsyms = *(uint32_t *)(uintptr_t)(base + d[1] + 4);
            break;
        case DT_GNU_HASH: gnu_hash = d[1]; break;
        case 12:
            NEED(d[1], 4);
            m->init = base + d[1];
            break;  // DT_INIT
        case 13:
            NEED(d[1], 4);
            m->fini = base + d[1];
            break;  // DT_FINI
        case DT_INIT_ARRAY:
            NEED(d[1], 8);
            m->init_array = base + d[1];
            break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d[1]; break;
        case DT_FINI_ARRAY:
            NEED(d[1], 8);
            m->fini_array = base + d[1];
            break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d[1]; break;
        }
    }
#undef NEED
    if (!m->nsyms && gnu_hash) m->nsyms = gnu_hash_nsyms(base, size, gnu_hash);
    if (m->symtab && m->nsyms) {  // (the symbol table's size comes from DT_HASH's nchain, or DT_GNU_HASH's chains)
        uint64_t off = m->symtab - base;
        if (m->nsyms > (size - off) / sizeof(Elf64Sym))
            FATAL("%s: bad symbol count %llu\n", path, (unsigned long long)m->nsyms);
    }
    if (m->strtab && (m->strtab < base || m->strtab >= base + size)) FATAL("%s: bad string table\n", path);
    if (m->init_array && (m->init_array < base || m->init_array_sz > base + size - m->init_array))
        FATAL("%s: init array past the module\n", path);
    if (m->fini_array && (m->fini_array < base || m->fini_array_sz > base + size - m->fini_array))
        FATAL("%s: fini array past the module\n", path);
    LOG("loaded %-5s at %llx size %llx syms %llu\n", m->name, base, size, m->nsyms);
}

static Elf64Sym *sym(Module *m, uint64_t i) { return (Elf64Sym *)m->symtab + i; }
static const char *symname(Module *m, Elf64Sym *s) { return (const char *)(m->strtab + s->name); }

Module *loader_module(const char *name) {
    for (int i = 0; i < g_nmodules; i++)
        if (!strcmp(g_modules[i].name, name)) return &g_modules[i];
    return NULL;
}

// Horizon's rtld looks symbols up in this order: main, the subsdk*s, sdk. That is also the interposition order:
// a game that exports operator new/delete (Unity does) preempts the SDK's copies for every module's references.
// NXRT_SYMBOL_PREEMPT=1 opts in; without it the lookup is the historical module-array order, so every other port
// resolves exactly as it did before this branch.
static int preempt_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("NXRT_SYMBOL_PREEMPT") ? 1 : 0;
    return v;
}

static int sym_class(const char *name) {
    if (!strcmp(name, "main")) return 0;
    if (!strncmp(name, "subsdk", 6)) return 1;
    return 2;
}

uint64_t loader_find_symbol(const char *name) {
    if (!preempt_on())
        for (int mi = 0; mi < g_nmodules; mi++) {
            Module *m = &g_modules[mi];
            for (uint64_t i = 1; i < m->nsyms; i++) {
                Elf64Sym *s = sym(m, i);
                if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
            }
        }
    else
        for (int pass = 0; pass < 3; pass++)
            for (int mi = 0; mi < g_nmodules; mi++) {
                Module *m = &g_modules[mi];
                if (sym_class(m->name) != pass) continue;
                for (uint64_t i = 1; i < m->nsyms; i++) {
                    Elf64Sym *s = sym(m, i);
                    if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
                }
            }
    return 0;
}

const char *loader_symbol_at(uint64_t addr, uint64_t *offset) {
    const char *best = NULL;
    uint64_t best_addr = 0;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (addr < m->base || addr >= m->base + m->size) continue;
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            uint64_t a = m->base + s->value;
            if (s->shndx && (s->info & 0xF) == 2 && a <= addr && a > best_addr) {
                best = symname(m, s);
                best_addr = a;
            }
        }
    }
    if (offset) *offset = addr - best_addr;
    return best;
}

static uint64_t resolve(Module *m, uint32_t symidx) {
    Elf64Sym *s = sym(m, symidx);
    const char *name = symname(m, s);
    if (s->shndx) {  // with preemption: an earlier module's export of the same name wins (rtld's order)
        if (preempt_on()) {
            uint64_t pre = loader_find_symbol(name);
            return pre ? pre : m->base + s->value;
        }
        return m->base + s->value;
    }
    HostFn impl = hle_lookup_impl(name);
    if (impl) return hle_register(name, impl);  // host implementation wins over the guest SDK's
    uint64_t a = loader_find_symbol(name);
    if (a) return a;
    if ((s->info >> 4) == 2) return 0;  // weak undefined
    return hle_register(name, NULL);
}

// DT_RELR, newer linkers' packed relative relocations (each word: += the module's base). An even entry is the offset of
// one word to relocate; an odd one is a bitmap of the 63 words after the last one (bit i: word i - 1). Returns how many.
static uint64_t relocate_relr(Module *m, uint64_t relr, uint64_t relrsz) {
    if (relr > m->size || relrsz > m->size - relr || relrsz % 8) FATAL("%s: bad DT_RELR table\n", m->name);
    const uint64_t *e = (const uint64_t *)(uintptr_t)(m->base + relr);
    uint64_t where = 0, n = 0;
#define RELR_AT(off)                                                                                                   \
    do {                                                                                                               \
        if ((off) > m->size - 8) FATAL("%s: DT_RELR entry past the module\n", m->name);                               \
        *(uint64_t *)(uintptr_t)(m->base + (off)) += m->base;                                                          \
        n++;                                                                                                           \
    } while (0)
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        if (!(e[i] & 1)) {
            RELR_AT(e[i]);
            where = e[i] + 8;
        } else {
            for (uint64_t bits = e[i] >> 1, k = 0; bits; bits >>= 1, k++)
                if (bits & 1) RELR_AT(where + k * 8);
            where += 63 * 8;
        }
    }
#undef RELR_AT
    return n;
}

static void relocate(Module *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        if (d[0] == DT_RELA) rela = m->base + d[1];
        if (d[0] == DT_RELASZ) relasz = d[1];
        if (d[0] == DT_JMPREL) jmprel = m->base + d[1];
        if (d[0] == DT_PLTRELSZ) pltsz = d[1];
        if (d[0] == DT_RELR) relr = d[1];
        if (d[0] == DT_RELRSZ) relrsz = d[1];
    }
    uint64_t packed = relrsz ? relocate_relr(m, relr, relrsz) : 0;
    int unresolved = 0;
    for (int pass = 0; pass < 2; pass++) {
        Elf64Rela *r = (Elf64Rela *)(pass ? jmprel : rela);
        uint64_t n = (pass ? pltsz : relasz) / sizeof(Elf64Rela);
        if (n && !r) FATAL("%s: relocation size without a table\n", m->name);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t *where = (uint64_t *)(m->base + r[i].off);
            uint32_t type = (uint32_t)r[i].info, si = r[i].info >> 32;
            switch (type) {
            case R_RELATIVE: *where = m->base + r[i].addend; break;
            case R_ABS64:
            case R_GLOB_DAT:
            case R_JUMP_SLOT: {
                if (type == R_ABS64 && si == 0) { *where = (uint64_t)r[i].addend; break; }  // (absolute, no symbol)
                uint64_t s = resolve(m, si);
                *where = s ? s + r[i].addend : 0;
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            default: FATAL("%s: reloc type %u\n", m->name, type);
            }
        }
    }
    LOG("relocated %-5s (%d imports bound to host stubs, %llu packed relative)\n", m->name, unresolved,
        (unsigned long long)packed);
}

void loader_load(const char *exefs) {
    char path[1024];
    for (int i = 0; i < nx_module_count(); i++) {  // (the game's modules: a missing file is left out)
        snprintf(path, sizeof path, "%s/%s", exefs, nx_module_name(i));
        FILE *f = fopen(path, "rb");
        if (!f) { LOG("[loader] no %s: left out\n", nx_module_name(i)); continue; }
        fclose(f);
        Module *m = &g_modules[g_nmodules++];
        m->name = nx_module_name(i);
        load_nso(m, path, nx_module_base_at(i));
        // (the next module's base, or the end of the modules' region: a bigger module would overwrite it)
        uint64_t next = i + 1 < nx_module_count() ? nx_module_base_at(i + 1) : HLE_BASE;
        if (m->base + m->size > next)
            FATAL("module %s (0x%llx bytes) overlaps the next one: fix nx_game.info.modules\n", m->name, (unsigned long long)m->size);
    }
    for (int i = 0; i < g_nmodules; i++) relocate(&g_modules[i]);
}

// ---- the game's modules (nx_game.info.modules)
static const NxModule k_default_modules[] = {{"main", 0x7000000000ull}, {"sdk", 0x7004000000ull}};
int nx_module_count(void) {
    int n = 0;
    while (n < NX_MAX_MODULES && nx_game.info.modules[n].name) n++;
    return n ? n : 2;
}
const char *nx_module_name(int i) { return nx_game.info.modules[0].name ? nx_game.info.modules[i].name : k_default_modules[i].name; }
uint64_t nx_module_base_at(int i) { return nx_game.info.modules[0].name ? nx_game.info.modules[i].base : k_default_modules[i].base; }
uint64_t nx_module_base(const char *name) {
    for (int i = 0; i < nx_module_count(); i++)
        if (!strcmp(nx_module_name(i), name)) return nx_module_base_at(i);
    return 0;
}
