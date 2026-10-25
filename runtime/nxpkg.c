// The player's game package read in place (nxpkg.h). Layout as in Atmosphere's fssystem (NcaFileSystemDriver,
// CompressedStorage, BucketTree) and Switchbrew's NCA / XCI / PFS0 / HFS0 pages, the same as tools/nca_extract.py:
//   package (NSP = PFS0 of NCAs; XCI = HFS0 root -> "secure" HFS0 of NCAs) -> program NCA (header: AES-XTS with
//   header_key) -> section (AES-CTR, key from the key area or the title key) -> hash layers -> [CompressedStorage]
//   -> PFS0 (ExeFS) / RomFS.
// Hashes are not verified (the files are the player's own dump; a bad one shows up as a game error).
#include "nxpkg.h"
#include "lz4_block.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arm_neon.h>


#define WHY(...) (snprintf(why, why_size, __VA_ARGS__), false)

// The package by path, or (Android) by the document URI the folder picker gave access to
static int open_package(const char *path) {
    return open(path, O_RDONLY);
}

// ------------------------------------------------------------------ AES-128 (software; ARMv8 AES for CTR)
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
static uint8_t INV_SBOX[256];

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a)) if (b & 1) r ^= a;
    return r;
}

static void init_inv(void) {
    for (int i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (uint8_t)i;
}

static void aes_expand(const uint8_t key[16], uint8_t rk[176]) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_inv);
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = SBOX[t[1]] ^ rcon, t[1] = SBOX[t[2]], t[2] = SBOX[t[3]], t[3] = SBOX[u];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = rk[i - 16 + k] ^ t[k];
    }
}
static void aes_encrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows (column-major)
        if (r < 10)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = t + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                q[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                q[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                q[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                q[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * r + i];
    }
}

static void aes_decrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = INV_SBOX[s[i]];  // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; i++) t[i] ^= rk[16 * r + i];
        if (r > 0)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = t + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                q[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                q[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                q[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                q[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        memcpy(s, t, 16);
    }
}

void nx_aes128_ecb_decrypt(const uint8_t key[16], uint8_t *data, size_t len) {
    uint8_t rk[176];
    aes_expand(key, rk);
    for (size_t i = 0; i + 16 <= len; i += 16) aes_decrypt_block(rk, data + i);
}

static void ctr_inc(uint8_t c[16]) {
    for (int i = 15; i >= 0 && !++c[i]; i--) {}
}

static bool have_arm_aes(void) {
    return true;  // every Apple Silicon CPU
}

__attribute__((target("aes"))) static void ctr_arm(const uint8_t rk[176], uint8_t ctr[16], uint8_t *data, size_t len) {
    uint8x16_t k[11];
    for (int i = 0; i < 11; i++) k[i] = vld1q_u8(rk + 16 * i);
    while (len) {
        uint8x16_t b = vld1q_u8(ctr);
        for (int r = 0; r < 9; r++) b = vaesmcq_u8(vaeseq_u8(b, k[r]));
        b = veorq_u8(vaeseq_u8(b, k[9]), k[10]);
        size_t n = len < 16 ? len : 16;
        if (n == 16) vst1q_u8(data, veorq_u8(vld1q_u8(data), b));
        else {
            uint8_t ks[16];
            vst1q_u8(ks, b);
            for (size_t i = 0; i < n; i++) data[i] ^= ks[i];
        }
        data += n, len -= n;
        ctr_inc(ctr);
    }
}

void nx_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t len) {
    uint8_t rk[176], ctr[16];
    aes_expand(key, rk);
    memcpy(ctr, iv, 16);
    static int arm = -1;
    if (arm < 0) arm = have_arm_aes();
    if (arm) { ctr_arm(rk, ctr, data, len); return; }
    while (len) {
        uint8_t ks[16];
        memcpy(ks, ctr, 16);
        aes_encrypt_block(rk, ks);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) data[i] ^= ks[i];
        data += n, len -= n;
        ctr_inc(ctr);
    }
}

// AES-XTS with Nintendo's tweak (the sector number big-endian), 0x200-byte sectors: the NCA header.
static void aes_xts_decrypt(const uint8_t key[32], uint8_t *data, size_t len, uint64_t sector) {
    uint8_t rk1[176], rk2[176];
    aes_expand(key, rk1);
    aes_expand(key + 16, rk2);
    for (size_t s = 0; s < len; s += 0x200, sector++) {
        uint8_t tw[16] = {0};
        for (int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(sector >> (8 * i));
        aes_encrypt_block(rk2, tw);
        for (size_t b = 0; b < 0x200; b += 16) {
            uint8_t *p = data + s + b;
            for (int i = 0; i < 16; i++) p[i] ^= tw[i];
            aes_decrypt_block(rk1, p);
            for (int i = 0; i < 16; i++) p[i] ^= tw[i];
            uint8_t carry = tw[15] >> 7;  // tweak * x in GF(2^128), little-endian bytes
            for (int i = 15; i > 0; i--) tw[i] = (uint8_t)(tw[i] << 1 | tw[i - 1] >> 7);
            tw[0] = (uint8_t)(tw[0] << 1) ^ (carry ? 0x87 : 0);
        }
    }
}

// ------------------------------------------------------------------ LZ4 block
int nx_lz4_decompress(const uint8_t *src, int src_len, uint8_t *dst, int dst_len) {
    if (src_len < 0 || dst_len < 0) return -1;
    size_t got = rt_lz4_decompress(src, (size_t)src_len, dst, (size_t)dst_len);
    return got == (size_t)dst_len ? (int)got : -1;
}

// ------------------------------------------------------------------ keys
typedef struct { char name[64]; uint8_t val[32]; int len; } Key;
typedef struct { Key *k; int n; } Keys;

static int unhex(const char *s, uint8_t *out, int cap) {
    int n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static bool keys_load(const char *path, Keys *ks) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char name[64], val[256];
        if (sscanf(line, " %63s", name) != 1 || sscanf(eq + 1, " %255s", val) != 1) continue;
        ks->k = realloc(ks->k, sizeof *ks->k * (ks->n + 1));
        Key *k = &ks->k[ks->n++];
        snprintf(k->name, sizeof k->name, "%s", name);
        for (char *c = k->name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        k->len = unhex(val, k->val, 32);
    }
    fclose(f);
    return true;
}

static const uint8_t *key_get(const Keys *ks, const char *name, int len) {
    for (int i = 0; i < ks->n; i++)
        if (!strcmp(ks->k[i].name, name) && ks->k[i].len == len) return ks->k[i].val;
    return NULL;
}

// ------------------------------------------------------------------ package -> program NCA
typedef struct { char name[256]; uint64_t off, size; } Entry;
typedef struct { Entry *e; int n; } Entries;

static void entries_add(Entries *es, const char *name, uint64_t off, uint64_t size) {
    es->e = realloc(es->e, sizeof *es->e * (es->n + 1));
    Entry *e = &es->e[es->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->off = off, e->size = size;
}

static bool pread_all(int fd, void *dst, size_t n, uint64_t off) {
    uint8_t *p = dst;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0) return false;
        p += r, n -= (size_t)r, off += (uint64_t)r;
    }
    return true;
}

// PFS0 (entry 0x18) / HFS0 (entry 0x40) at `base`: its files, offsets absolute in the package
static bool read_fs0(int fd, uint64_t base, Entries *out) {
    uint8_t h[16];
    if (!pread_all(fd, h, 16, base)) return false;
    bool hfs = !memcmp(h, "HFS0", 4);
    if (memcmp(h, "PFS0", 4) && !hfs) return false;
    uint32_t n, strsz;
    memcpy(&n, h + 4, 4), memcpy(&strsz, h + 8, 4);
    if (n > 4096 || strsz > (1u << 20)) return false;
    size_t esz = hfs ? 0x40 : 0x18, meta = 16 + n * esz + strsz;
    uint8_t *m = malloc(meta);
    if (m && !pread_all(fd, m, meta, base)) {
        free(m);
        return false;
    }
    if (!m) return false;
    const char *strs = (const char *)m + 16 + n * esz;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off, size;
        uint32_t no;
        memcpy(&off, m + 16 + i * esz, 8), memcpy(&size, m + 16 + i * esz + 8, 8), memcpy(&no, m + 16 + i * esz + 16, 4);
        if (no >= strsz) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)strnlen(strs + no, strsz - no), strs + no);
        entries_add(out, name, base + meta + off, size);
    }
    free(m);
    return true;
}

typedef struct {
    int fd;
    uint64_t base;      // the NCA's offset in the package
    uint8_t hdr[0xC00];  // decrypted header
    uint8_t ctr_key[16];
} Nca;

typedef struct {
    uint64_t off, size;           // in the NCA
    uint8_t fs_type, hash_type, enc_type;
    uint8_t ctr_hi[8];
    uint64_t data_off, data_size;  // the payload under the hash layers, relative to the section
    uint64_t comp_off, comp_size;  // compression table, relative to the payload
    uint8_t comp_hdr[16];
} Section;

static bool nca_header(int fd, uint64_t base, const Keys *ks, Nca *nca) {
    const uint8_t *hk = key_get(ks, "header_key", 32);
    if (!hk) return false;
    nca->fd = fd, nca->base = base;
    if (!pread_all(fd, nca->hdr, 0xC00, base)) return false;
    aes_xts_decrypt(hk, nca->hdr, 0xC00, 0);
    return !memcmp(nca->hdr + 0x200, "NCA3", 4);
}

static bool section_info(const Nca *nca, int i, Section *s) {
    uint32_t st, en;
    memcpy(&st, nca->hdr + 0x240 + i * 16, 4), memcpy(&en, nca->hdr + 0x244 + i * 16, 4);
    if (st == 0 && en == 0) return false;
    if (en < st) return false;  // (a corrupt header: the size would wrap)
    const uint8_t *fh = nca->hdr + 0x400 + i * 0x200;
    memset(s, 0, sizeof *s);
    s->off = (uint64_t)st * 0x200, s->size = (uint64_t)(en - st) * 0x200;
    s->fs_type = fh[2], s->hash_type = fh[3], s->enc_type = fh[4];
    for (int k = 0; k < 8; k++) s->ctr_hi[k] = fh[0x147 - k];
    if (s->hash_type == 2) {  // HierarchicalSha256: the last layer region is the data
        uint32_t layers;
        memcpy(&layers, fh + 0x8 + 0x24, 4);
        if (layers < 1 || layers > 5) return false;
        memcpy(&s->data_off, fh + 0x8 + 0x28 + (layers - 1) * 16, 8);
        memcpy(&s->data_size, fh + 0x8 + 0x30 + (layers - 1) * 16, 8);
    } else if (s->hash_type == 3) {  // HierarchicalIntegrity (IVFC): the last level is the data
        uint32_t levels;
        if (memcmp(fh + 0x8, "IVFC", 4)) return false;
        memcpy(&levels, fh + 0x8 + 0xC, 4);
        if (levels < 2 || levels > 7) return false;
        memcpy(&s->data_off, fh + 0x8 + 0x10 + (levels - 2) * 0x18, 8);
        memcpy(&s->data_size, fh + 0x8 + 0x18 + (levels - 2) * 0x18, 8);
    } else return false;
    memcpy(&s->comp_off, fh + 0x178, 8), memcpy(&s->comp_size, fh + 0x180, 8);
    memcpy(s->comp_hdr, fh + 0x188, 16);
    // The hash layer's data and the compression table live inside the section.
    if (s->data_off > s->size || s->data_size > s->size - s->data_off) return false;
    if (s->comp_size && (s->comp_off > s->data_size || s->comp_size > s->data_size - s->comp_off)) return false;
    return true;
}

// decrypted bytes [off, off+n) of a section (off relative to the section)
static bool section_read(int fd, uint64_t nca_base, const Section *s, const uint8_t key[16], uint8_t *dst, uint64_t off,
                         size_t n) {
    uint64_t a = off & ~15ull, pre = off - a;
    if (s->enc_type == 1) return pread_all(fd, dst, n, nca_base + s->off + off);
    if (s->enc_type != 3) return false;
    uint8_t *tmp = pre ? malloc(n + pre) : dst;
    if (!tmp) return false;
    if (!pread_all(fd, tmp, n + pre, nca_base + s->off + a)) { if (pre) free(tmp); return false; }
    uint8_t iv[16];
    memcpy(iv, s->ctr_hi, 8);
    uint64_t blk = (s->off + a) >> 4;
    for (int k = 0; k < 8; k++) iv[15 - k] = (uint8_t)(blk >> (8 * k));
    nx_aes128_ctr(key, iv, tmp, n + pre);
    if (pre) memcpy(dst, tmp + pre, n), free(tmp);
    return true;
}

// ------------------------------------------------------------------ CompressedStorage
// BucketTree of CompressedStorage::Entry {s64 virt; s64 phys; u8 type; s8 level; u16; u32 phys_size}, node size
// 0x4000. Types: 0 none (copy), 1 zeros, 3 LZ4.
typedef struct { int64_t virt, phys; uint8_t type; uint32_t psize; } CEntry;
typedef struct { uint8_t *data; uint64_t entry; uint32_t len; uint64_t stamp; } CBlock;
#define NCACHE 16

struct NxRomfs {
    int fd;
    uint64_t nca_base;
    Section sec;
    uint8_t key[16];
    uint64_t size;  // the RomFS image's
    CEntry *ent;
    uint64_t nent;
    pthread_mutex_t lock;
    CBlock cache[NCACHE];
    uint64_t stamp;
};

static bool comp_load(NxRomfs *r, char *why, size_t why_size) {
    const uint8_t *h = r->sec.comp_hdr;
    if (memcmp(h, "BKTR", 4)) return WHY("bad compression table header");
    int32_t count;
    memcpy(&count, h + 8, 4);
    const uint64_t NODE = 0x4000, ENTRY = 0x18;
    // A corrupt table must fail here, before anything indexes it: there is no entry without its 0x18 bytes.
    if (count <= 0 || (uint64_t)count > r->sec.comp_size / ENTRY)
        return WHY("bad compression table entry count (%d)", count);
    if (!r->sec.comp_size) return WHY("empty compression table");
    uint8_t *table = malloc(r->sec.comp_size);
    if (!table) return WHY("out of memory for the compression table");
    if (!section_read(r->fd, r->nca_base, &r->sec, r->key, table, r->sec.data_off + r->sec.comp_off, r->sec.comp_size)) {
        free(table);
        return WHY("can't read the compression table (package cut short?)");
    }
    uint64_t per_set = (NODE - 0x10) / ENTRY, sets = ((uint64_t)count + per_set - 1) / per_set,
             per_node = (NODE - 0x10) / 8;
    uint64_t l2 = sets <= per_node ? 0 : (sets - per_node + per_node - 2) / (per_node - 1);
    uint64_t node_storage = (1 + l2) * NODE;
    r->ent = calloc((size_t)count + 1, sizeof *r->ent);
    if (!r->ent) {
        free(table);
        return WHY("out of memory for the compression table");
    }
    r->nent = 0;
    int64_t end = 0, prev_virt = 0;
    for (uint64_t s = 0; s < sets; s++) {
        uint64_t b = node_storage + s * NODE;
        if (b + NODE > r->sec.comp_size) { free(table); return WHY("compression table cut short"); }
        int32_t idx, n;
        memcpy(&idx, table + b, 4), memcpy(&n, table + b + 4, 4), memcpy(&end, table + b + 8, 8);
        if ((uint64_t)idx != s || n < 0 || (uint64_t)n > per_set) { free(table); return WHY("bad compression table"); }
        for (int e = 0; e < n && r->nent < (uint64_t)count; e++) {
            const uint8_t *p = table + b + 0x10 + e * ENTRY;
            CEntry *c = &r->ent[r->nent++];
            memcpy(&c->virt, p, 8), memcpy(&c->phys, p + 8, 8), c->type = p[16], memcpy(&c->psize, p + 20, 4);
            // Virtual offsets grow; a physical block is inside the section; a type we know is 0, 1 or 3.
            if (c->virt < prev_virt || c->phys < 0 || c->psize > r->sec.size ||
                (c->type != 0 && c->type != 1 && c->type != 3)) {
                free(table);
                return WHY("bad compression table entry %llu", (unsigned long long)(r->nent - 1));
            }
            prev_virt = c->virt;
        }
    }
    free(table);
    if (r->nent != (uint64_t)count) return WHY("compression table: %llu entries of %d", (unsigned long long)r->nent, count);
    if (r->ent[0].virt != 0) return WHY("compression table doesn't start at 0");
    if (end < prev_virt) return WHY("compression table: the last block ends before it starts");
    r->ent[r->nent].virt = end;  // (the sentinel: the end of the last entry)
    r->size = (uint64_t)end;
    return true;
}

// the cached, decompressed block of entry i
static const uint8_t *comp_block(NxRomfs *r, uint64_t i, uint32_t *len) {
    CBlock *victim = &r->cache[0];
    for (int k = 0; k < NCACHE; k++) {
        CBlock *c = &r->cache[k];
        if (c->data && c->entry == i) { c->stamp = ++r->stamp; *len = c->len; return c->data; }
        if (!c->data || c->stamp < victim->stamp) victim = c;
    }
    if (i >= r->nent) return NULL;
    const CEntry *e = &r->ent[i];
    int64_t vend = r->ent[i + 1].virt;
    if (vend < e->virt) return NULL;
    uint64_t vlen64 = (uint64_t)(vend - e->virt);
    if (vlen64 > r->size || vlen64 > 0xFFFFFFFFull || e->psize > r->sec.size) return NULL;
    uint32_t vlen = (uint32_t)vlen64;
    uint8_t *raw = malloc(e->psize ? e->psize : 1), *out = malloc(vlen ? vlen : 1);
    if (!raw || !out) {
        free(raw), free(out);
        return NULL;
    }
    if (!section_read(r->fd, r->nca_base, &r->sec, r->key, raw, r->sec.data_off + (uint64_t)e->phys, e->psize) ||
        nx_lz4_decompress(raw, (int)e->psize, out, (int)vlen) != (int)vlen) {
        free(raw), free(out);
        return NULL;
    }
    free(raw);
    free(victim->data);
    victim->data = out, victim->entry = i, victim->len = vlen, victim->stamp = ++r->stamp;
    *len = vlen;
    return out;
}

ssize_t nx_romfs_pread(NxRomfs *r, void *dst_, uint64_t size, uint64_t off) {
    uint8_t *dst = dst_;
    if (off >= r->size) return 0;
    if (size > r->size - off) size = r->size - off;
    if (!r->ent)
        return section_read(r->fd, r->nca_base, &r->sec, r->key, dst, r->sec.data_off + off, size) ? (ssize_t)size : -1;
    pthread_mutex_lock(&r->lock);
    uint64_t lo = 0, hi = r->nent;  // last entry with virt <= off
    while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        if ((uint64_t)r->ent[mid].virt <= off) lo = mid;
        else hi = mid;
    }
    uint64_t done = 0;
    bool ok = true;
    for (uint64_t i = lo; done < size && i < r->nent && ok; i++) {
        const CEntry *e = &r->ent[i];
        uint64_t v = (uint64_t)e->virt, vend = (uint64_t)r->ent[i + 1].virt, o = off + done;
        if (o >= vend) continue;
        uint64_t n = vend - o < size - done ? vend - o : size - done;
        if (e->type == 0) ok = section_read(r->fd, r->nca_base, &r->sec, r->key, dst + done,
                                            r->sec.data_off + (uint64_t)e->phys + (o - v), n);
        else if (e->type == 1) memset(dst + done, 0, n);
        else if (e->type == 3) {
            uint32_t len;
            const uint8_t *b = comp_block(r, i, &len);
            if (b) memcpy(dst + done, b + (o - v), n);
            else ok = false;
        } else ok = false;
        done += n;
    }
    pthread_mutex_unlock(&r->lock);
    return ok ? (ssize_t)done : -1;
}

uint64_t nx_romfs_size(const NxRomfs *r) { return r->size; }

void nx_romfs_close(NxRomfs *r) {
    if (!r) return;
    for (int k = 0; k < NCACHE; k++) free(r->cache[k].data);
    free(r->ent);
    if (r->fd >= 0) close(r->fd);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

// ------------------------------------------------------------------ romfs.src
// One "name=value" per line: package (path), size (its size in bytes: a changed file is refused), nca (the program
// NCA's offset in it), section (offset, size, enc, ctr_hi), key, data (offset, size), comp (offset, size, header).
static void hex(char *out, const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

NxRomfs *nx_romfs_open(const char *src_path, char *why, size_t why_size) {
    FILE *f = fopen(src_path, "r");
    if (!f) { snprintf(why, why_size, "no %s", src_path); return NULL; }
    NxRomfs *r = calloc(1, sizeof *r);
    r->fd = -1;
    pthread_mutex_init(&r->lock, NULL);
    char line[2048], package[1800] = "";
    unsigned long long pkg_size = 0, a, b;
    unsigned enc;
    char h1[64], h2[64];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "package=", 8)) snprintf(package, sizeof package, "%s", line + 8);
        else if (sscanf(line, "size=%llu", &a) == 1) pkg_size = a;
        else if (sscanf(line, "nca=%llu", &a) == 1) r->nca_base = a;
        else if (sscanf(line, "section=%llu %llu %u %63s", &a, &b, &enc, h1) == 4)
            r->sec.off = a, r->sec.size = b, r->sec.enc_type = (uint8_t)enc, unhex(h1, r->sec.ctr_hi, 8);
        else if (sscanf(line, "key=%63s", h1) == 1) unhex(h1, r->key, 16);
        else if (sscanf(line, "data=%llu %llu", &a, &b) == 2) r->sec.data_off = a, r->sec.data_size = b;
        else if (sscanf(line, "comp=%llu %llu %63s", &a, &b, h2) == 3)
            r->sec.comp_off = a, r->sec.comp_size = b, unhex(h2, r->sec.comp_hdr, 16);
    }
    fclose(f);
    struct stat sb;
    r->fd = package[0] ? open_package(package) : -1;
    if (r->fd < 0 || fstat(r->fd, &sb) || (unsigned long long)sb.st_size != pkg_size) {
        snprintf(why, why_size, "the game package moved or changed (%s)", package);
        nx_romfs_close(r);
        return NULL;
    }
    r->size = r->sec.data_size;
    if (r->sec.comp_size && !comp_load(r, why, why_size)) { nx_romfs_close(r); return NULL; }
    return r;
}

// ------------------------------------------------------------------ setup
static bool title_key(const Nca *nca, int fd, const Entries *files, const Keys *ks, const char *keys_path, int keygen,
                      uint8_t out[16]) {
    const uint8_t *rights = nca->hdr + 0x230;
    uint8_t enc[16];
    bool found = false;
    for (int i = 0; i < files->n && !found; i++) {  // the NSP's ticket: <rights id>.tik, title key block at 0x180
        size_t l = strlen(files->e[i].name);
        if (l < 4 || strcmp(files->e[i].name + l - 4, ".tik") || files->e[i].size < 0x2B0) continue;
        uint8_t tik[0x2B0];
        if (pread_all(fd, tik, sizeof tik, files->e[i].off) && !memcmp(tik + 0x2A0, rights, 16))
            memcpy(enc, tik + 0x180, 16), found = true;
    }
    if (!found) {  // title.keys beside prod.keys: <rights id> = <encrypted title key>
        char tk[1200], name[33];
        snprintf(tk, sizeof tk, "%s", keys_path);
        char *sl = strrchr(tk, '/');
        snprintf(sl ? sl + 1 : tk, sizeof tk - (sl ? (size_t)(sl + 1 - tk) : 0), "title.keys");
        Keys t = {0};
        hex(name, rights, 16);
        if (keys_load(tk, &t)) {
            const uint8_t *v = key_get(&t, name, 16);
            if (v) memcpy(enc, v, 16), found = true;
        }
        free(t.k);
    }
    if (!found) return false;
    char kn[32];
    snprintf(kn, sizeof kn, "titlekek_%02x", keygen);
    const uint8_t *kek = key_get(ks, kn, 16);
    if (!kek) return false;
    memcpy(out, enc, 16);
    nx_aes128_ecb_decrypt(kek, out, 16);
    return true;
}

static bool write_file(const char *path, int fd, uint64_t nca_base, const Section *s, const uint8_t key[16], uint64_t off,
                       uint64_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t *buf = malloc(4 << 20);
    bool ok = true;
    for (uint64_t done = 0; done < size && ok;) {
        size_t n = size - done < (4u << 20) ? (size_t)(size - done) : (4u << 20);
        ok = section_read(fd, nca_base, s, key, buf, off + done, n) && fwrite(buf, 1, n, f) == n;
        done += n;
    }
    free(buf);
    return fclose(f) == 0 && ok;
}

// The package's files (an NSP's PFS0, an XCI's secure partition, or a bare NCA as one file). false: why.
static bool package_files(int fd, Entries *files, char *why, size_t why_size) {
    struct stat sb;
    if (fstat(fd, &sb)) return WHY("can't open the game package");
    uint8_t head[0x200];
    if (!pread_all(fd, head, sizeof head, 0)) return WHY("the game package is too small");
    if (!memcmp(head, "PFS0", 4)) {  // NSP
        if (!read_fs0(fd, 0, files)) return WHY("bad NSP");
    } else if (!memcmp(head + 0x100, "HEAD", 4)) {  // XCI: the root HFS0's "secure" partition
        uint64_t root;
        memcpy(&root, head + 0x130, 8);
        Entries parts = {0};
        if (!read_fs0(fd, root, &parts)) return WHY("bad XCI");
        for (int i = 0; i < parts.n; i++)
            if (!strcmp(parts.e[i].name, "secure")) read_fs0(fd, parts.e[i].off, files);
        free(parts.e);
        if (!files->n) return WHY("XCI without a secure partition");
    } else {
        entries_add(files, "program.nca", 0, (uint64_t)sb.st_size);  // a bare NCA
    }
    for (int i = 0; i < files->n; i++)
        if (files->e[i].off + files->e[i].size > (uint64_t)sb.st_size)
            return WHY("the game package is incomplete (%llu MB missing): copy it again",
                       (unsigned long long)((files->e[i].off + files->e[i].size - (uint64_t)sb.st_size) >> 20));
    return true;
}

// The first NCA of the package with this content type (0 program, 2 control), its header decrypted. false: none (*any:
// whether any NCA's header decrypted at all - wrong keys otherwise).
static bool find_nca(int fd, const Entries *files, const Keys *ks, int type, Nca *nca, bool *any) {
    for (int i = 0; i < files->n; i++) {
        size_t l = strlen(files->e[i].name);
        if (l < 4 || strcmp(files->e[i].name + l - 4, ".nca")) continue;
        if (!nca_header(fd, files->e[i].off, ks, nca)) continue;
        if (any) *any = true;
        if (nca->hdr[0x205] == type) return true;
    }
    return false;
}

// The NCA's section key (AES-CTR) into nca->ctr_key: its title key (a ticket in the NSP, or title.keys beside the
// keys file) or its key area. false: why.
static bool nca_key(Nca *nca, int fd, const Entries *files, const Keys *ks, const char *keys_path, char *why, size_t why_size) {
    int gen = nca->hdr[0x206] > nca->hdr[0x220] ? nca->hdr[0x206] : nca->hdr[0x220];
    int keygen = gen ? gen - 1 : 0;
    static const uint8_t zero[16];
    if (memcmp(nca->hdr + 0x230, zero, 16)) {
        if (!title_key(nca, fd, files, ks, keys_path, keygen, nca->ctr_key))
            return WHY("the package needs a title key (no ticket in it, none in title.keys)");
        return true;
    }
    char kn[48];
    snprintf(kn, sizeof kn, "key_area_key_application_%02x", keygen);
    const uint8_t *kaek = key_get(ks, kn, 16);
    if (!kaek) return WHY("the keys file has no %s (keys from an older system?)", kn);
    uint8_t area[0x40];
    memcpy(area, nca->hdr + 0x300, 0x40);
    nx_aes128_ecb_decrypt(kaek, area, 0x40);
    memcpy(nca->ctr_key, area + 0x20, 16);  // index 2: AES-CTR
    return true;
}

bool nx_setup(const char *package, const char *keys_path, const char *out_dir, uint64_t *title_id, char *why,
              size_t why_size) {
    Keys ks = {0};
    if (!keys_load(keys_path, &ks)) return WHY("can't read the keys file");
    bool ok = false;
    Entries files = {0};
    int fd = open_package(package);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb)) { WHY("can't open the game package"); goto out; }
    if (!package_files(fd, &files, why, why_size)) goto out;
    Nca nca;
    bool any_nca = false;
    if (!find_nca(fd, &files, &ks, 0, &nca, &any_nca)) {  // content type 0: the program
        WHY(any_nca ? "no game program in the package" : "can't decrypt the package (wrong or old keys?)");
        goto out;
    }
    uint64_t tid;
    memcpy(&tid, nca.hdr + 0x210, 8);
    if (title_id) *title_id = tid;
    if (!nca_key(&nca, fd, &files, &ks, keys_path, why, why_size)) goto out;
    Section exefs, romfs;
    if (!section_info(&nca, 0, &exefs) || !section_info(&nca, 1, &romfs) || exefs.fs_type != 1) {
        WHY("unexpected program layout");
        goto out;
    }
    // ExeFS: a PFS0 of the NSOs and main.npdm
    {
        uint8_t h[16];
        if (!section_read(fd, nca.base, &exefs, nca.ctr_key, h, exefs.data_off, 16) || memcmp(h, "PFS0", 4)) {
            WHY("can't decrypt the game (wrong keys?)");
            goto out;
        }
        uint32_t n, strsz;
        memcpy(&n, h + 4, 4), memcpy(&strsz, h + 8, 4);
        if (n > 4096 || strsz > (1u << 20)) {
            WHY("bad ExeFS table");
            goto out;
        }
        size_t meta = 16 + (size_t)n * 0x18 + strsz;
        uint8_t *m = malloc(meta);
        if (!m) {
            WHY("out of memory for the ExeFS table");
            goto out;
        }
        if (!section_read(fd, nca.base, &exefs, nca.ctr_key, m, exefs.data_off, meta)) {
            free(m);
            WHY("can't read the ExeFS table");
            goto out;
        }
        char dir[1200];
        snprintf(dir, sizeof dir, "%s/exefs", out_dir);
        mkdir(out_dir, 0755), mkdir(dir, 0755);
        bool wrote = true;
        for (uint32_t i = 0; i < n && wrote; i++) {
            uint64_t off, size;
            uint32_t no;
            memcpy(&off, m + 16 + i * 0x18, 8), memcpy(&size, m + 24 + i * 0x18, 8), memcpy(&no, m + 32 + i * 0x18, 4);
            if (no >= strsz) continue;
            const char *name = (const char *)m + 16 + n * 0x18 + no;
            if (strchr(name, '/') || strchr(name, '\\') || name[0] == '.') continue;
            char path[1400];
            snprintf(path, sizeof path, "%s/%.*s", dir, (int)strnlen(name, strsz - no), name);
            wrote = write_file(path, fd, nca.base, &exefs, nca.ctr_key, exefs.data_off + meta + off, size);
        }
        free(m);
        if (!wrote) { WHY("can't write the game code files (disk full?)"); goto out; }
    }
    // RomFS: described, not copied
    {
        char path[1200], tmp[1300], ctr[17], key[33], comp[33];
        snprintf(path, sizeof path, "%s/romfs.src", out_dir);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        hex(ctr, romfs.ctr_hi, 8), hex(key, nca.ctr_key, 16), hex(comp, romfs.comp_hdr, 16);
        FILE *f = fopen(tmp, "w");
        if (!f) { WHY("can't write %s", path); goto out; }
        fprintf(f, "package=%s\nsize=%llu\nnca=%llu\nsection=%llu %llu %u %s\nkey=%s\ndata=%llu %llu\ncomp=%llu %llu %s\n",
                package, (unsigned long long)sb.st_size, (unsigned long long)nca.base, (unsigned long long)romfs.off,
                (unsigned long long)romfs.size, romfs.enc_type, ctr, key, (unsigned long long)romfs.data_off,
                (unsigned long long)romfs.data_size, (unsigned long long)romfs.comp_off, (unsigned long long)romfs.comp_size,
                comp);
        if (fclose(f) || rename(tmp, path)) { WHY("can't write %s", path); goto out; }
        NxRomfs *r = nx_romfs_open(path, why, why_size);  // (checks the compression table and the RomFS header)
        if (!r) goto out;
        uint64_t hsize = 0;
        ok = nx_romfs_pread(r, &hsize, 8, 0) == 8 && hsize == 0x50;
        nx_romfs_close(r);
        if (!ok) { WHY("the game data doesn't decrypt (wrong keys?)"); goto out; }
    }
out:
    if (fd >= 0) close(fd);
    free(files.e);
    free(ks.k);
    return ok;
}

// ------------------------------------------------------------------ the RomFS's own tables: paths -> file data
// Header: ten u64 (header size, dir hash off/size, dir meta off/size, file hash off/size, file meta off/size, data off).
// Directory entry: parent, sibling, first child dir, first file, hash next, name length (u32 each), name. File entry:
// parent, sibling (u32), data offset, size (u64), hash next, name length (u32), name. Offsets of entries in their table;
// 0xFFFFFFFF ends a list.
typedef struct { uint64_t h[10]; uint8_t *dirs, *files; } RomTables;

static bool rom_tables(NxRomRead rd, void *rc, RomTables *t) {
    memset(t, 0, sizeof *t);
    if (rd(rc, t->h, sizeof t->h, 0) != (ssize_t)sizeof t->h) return false;
    if (t->h[4] > (64u << 20) || t->h[8] > (256u << 20)) return false;  // (a corrupt header: not GBs of tables)
    t->dirs = malloc(t->h[4] + 1), t->files = malloc(t->h[8] + 1);
    if (!t->dirs || !t->files || rd(rc, t->dirs, t->h[4], t->h[3]) != (ssize_t)t->h[4] ||
        rd(rc, t->files, t->h[8], t->h[7]) != (ssize_t)t->h[8]) {
        free(t->dirs), free(t->files);
        return false;
    }
    return true;
}

static uint32_t rom_u32(const uint8_t *tab, uint64_t size, uint64_t o) {
    uint32_t v = 0xFFFFFFFFu;
    if (o + 4 <= size) memcpy(&v, tab + o, 4);
    return v;
}

// the entry's name (at name_at in it, its length just before), if it fits the table
static bool rom_name(const uint8_t *tab, uint64_t size, uint64_t o, uint64_t name_at, const char **name, uint32_t *len) {
    if (o + name_at > size) return false;
    memcpy(len, tab + o + name_at - 4, 4);
    if (o + name_at + *len > size) return false;
    *name = (const char *)tab + o + name_at;
    return true;
}

bool nx_romtab_find(NxRomRead rd, void *rc, const char *path, uint64_t *off, uint64_t *size) {
    RomTables t;
    if (!rom_tables(rd, rc, &t)) return false;
    bool found = false;
    uint32_t dir = 0;  // (the root)
    for (const char *p = path; !found;) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        const char *name;
        uint32_t len;
        if (!slash) {  // the file, among the directory's files
            for (uint32_t f = rom_u32(t.dirs, t.h[4], (uint64_t)dir + 12); f != 0xFFFFFFFFu; f = rom_u32(t.files, t.h[8], (uint64_t)f + 4))
                if (rom_name(t.files, t.h[8], f, 0x20, &name, &len) && len == n && !memcmp(name, p, n)) {
                    memcpy(off, t.files + f + 8, 8), memcpy(size, t.files + f + 0x10, 8);
                    *off += t.h[9];
                    found = true;
                    break;
                }
            break;
        }
        uint32_t d = rom_u32(t.dirs, t.h[4], (uint64_t)dir + 8);
        while (d != 0xFFFFFFFFu && !(rom_name(t.dirs, t.h[4], d, 0x18, &name, &len) && len == n && !memcmp(name, p, n)))
            d = rom_u32(t.dirs, t.h[4], (uint64_t)d + 4);
        if (d == 0xFFFFFFFFu) break;
        dir = d;
        p = slash + 1;
    }
    free(t.dirs), free(t.files);
    return found;
}

static int rom_walk_dir(RomTables *t, uint32_t dir, char *path, size_t at, size_t cap, int depth,
                        void (*fn)(const char *, uint64_t, uint64_t, void *), void *ctx) {
    int count = 0;
    const char *name;
    uint32_t len;
    for (uint32_t f = rom_u32(t->dirs, t->h[4], (uint64_t)dir + 12); f != 0xFFFFFFFFu; f = rom_u32(t->files, t->h[8], (uint64_t)f + 4)) {
        if (!rom_name(t->files, t->h[8], f, 0x20, &name, &len) || at + len + 1 > cap) continue;
        memcpy(path + at, name, len), path[at + len] = 0;
        uint64_t off, size;
        memcpy(&off, t->files + f + 8, 8), memcpy(&size, t->files + f + 0x10, 8);
        fn(path, off + t->h[9], size, ctx);
        count++;
    }
    if (depth > 64) return count;  // (a looping table)
    for (uint32_t d = rom_u32(t->dirs, t->h[4], (uint64_t)dir + 8); d != 0xFFFFFFFFu; d = rom_u32(t->dirs, t->h[4], (uint64_t)d + 4)) {
        if (!rom_name(t->dirs, t->h[4], d, 0x18, &name, &len) || at + len + 2 > cap) continue;
        memcpy(path + at, name, len), path[at + len] = '/', path[at + len + 1] = 0;
        count += rom_walk_dir(t, d, path, at + len + 1, cap, depth + 1, fn, ctx);
    }
    return count;
}

int nx_romtab_walk(NxRomRead rd, void *rc, void (*fn)(const char *path, uint64_t off, uint64_t size, void *ctx), void *ctx) {
    RomTables t;
    if (!rom_tables(rd, rc, &t)) return -1;
    char path[1024] = "";
    int n = rom_walk_dir(&t, 0, path, 0, sizeof path, 0, fn, ctx);
    free(t.dirs), free(t.files);
    return n;
}

static ssize_t pkg_rd(void *rc, void *dst, uint64_t n, uint64_t off) { return nx_romfs_pread((NxRomfs *)rc, dst, n, off); }
bool nx_romfs_find(NxRomfs *r, const char *path, uint64_t *off, uint64_t *size) { return nx_romtab_find(pkg_rd, r, path, off, size); }
int nx_romfs_walk(NxRomfs *r, void (*fn)(const char *path, uint64_t off, uint64_t size, void *ctx), void *ctx) {
    return nx_romtab_walk(pkg_rd, r, fn, ctx);
}

// ------------------------------------------------------------------ what a package is: its program and control data
typedef struct { int fd; uint64_t base; const Section *s; const uint8_t *key; } SecRd;
static ssize_t sec_rd(void *rc, void *dst, uint64_t n, uint64_t off) {
    const SecRd *r = rc;
    if (off > r->s->data_size || n > r->s->data_size - off) return -1;
    return section_read(r->fd, r->base, r->s, r->key, dst, r->s->data_off + off, (size_t)n) ? (ssize_t)n : -1;
}

typedef struct { char best[64]; } IconPick;
static void pick_icon(const char *path, uint64_t off, uint64_t size, void *c) {
    (void)off, (void)size;
    IconPick *p = c;
    if (strncmp(path, "icon_", 5) || strchr(path, '/')) return;
    if (!p->best[0] || !strcmp(path, "icon_AmericanEnglish.dat")) snprintf(p->best, sizeof p->best, "%s", path);
}

static void nacp_text(char *out, size_t cap, const uint8_t *src, size_t n) {
    size_t k = 0;
    for (; k < n && k + 1 < cap && src[k]; k++) out[k] = (char)src[k];
    out[k] = 0;
}

bool nx_package_info(const char *package, const char *keys_path, NxPackageInfo *out, char *why, size_t why_size) {
    memset(out, 0, sizeof *out);
    Keys ks = {0};
    if (!keys_load(keys_path, &ks)) return WHY("can't read the keys file");
    bool ok = false;
    Entries files = {0};
    int fd = open_package(package);
    if (fd < 0) { WHY("can't open the game package"); goto out; }
    if (!package_files(fd, &files, why, why_size)) goto out;
    Nca prog, ctl;
    bool any = false;
    if (!find_nca(fd, &files, &ks, 0, &prog, &any)) {
        WHY(any ? "no game program in the package (an update or DLC?)" : "can't decrypt the package (wrong or old keys?)");
        goto out;
    }
    memcpy(&out->title_id, prog.hdr + 0x210, 8);
    // the main NSO's build id, from the program's ExeFS
    if (nca_key(&prog, fd, &files, &ks, keys_path, why, why_size)) {
        Section exefs;
        uint8_t h[16];
        if (section_info(&prog, 0, &exefs) && exefs.fs_type == 1 &&
            section_read(fd, prog.base, &exefs, prog.ctr_key, h, exefs.data_off, 16) && !memcmp(h, "PFS0", 4)) {
            uint32_t n, strsz;
            memcpy(&n, h + 4, 4), memcpy(&strsz, h + 8, 4);
            size_t meta = 16 + (size_t)n * 0x18 + strsz;
            uint8_t *m = n <= 4096 && strsz <= (1u << 20) ? malloc(meta) : NULL;
            if (m && section_read(fd, prog.base, &exefs, prog.ctr_key, m, exefs.data_off, meta))
                for (uint32_t i = 0; i < n; i++) {
                    uint64_t off;
                    uint32_t no;
                    memcpy(&off, m + 16 + i * 0x18, 8), memcpy(&no, m + 32 + i * 0x18, 4);
                    if (no >= strsz || strncmp((const char *)m + 16 + n * 0x18 + no, "main", strsz - no) ||
                        ((const char *)m)[16 + n * 0x18 + no + 4] != 0)
                        continue;
                    uint8_t nso[0x48];
                    if (section_read(fd, prog.base, &exefs, prog.ctr_key, nso, exefs.data_off + meta + off, sizeof nso) &&
                        !memcmp(nso, "NSO0", 4))
                        memcpy(out->build_id, nso + 0x40, 8), out->has_build_id = true;
                    break;
                }
            free(m);
        }
    }
    // the control data: names, version, icon (control.nacp and icon_<language>.dat in the control NCA's RomFS)
    why[0] = 0;
    if (find_nca(fd, &files, &ks, 2, &ctl, NULL) && nca_key(&ctl, fd, &files, &ks, keys_path, why, why_size)) {
        Section rs;
        if (section_info(&ctl, 0, &rs) && !rs.comp_size) {
            SecRd rd = {fd, ctl.base, &rs, ctl.ctr_key};
            uint64_t off, size;
            if (nx_romtab_find(sec_rd, &rd, "control.nacp", &off, &size) && size >= 0x3070) {
                uint8_t *nacp = malloc(0x3070);
                if (nacp && sec_rd(&rd, nacp, 0x3070, off) == 0x3070) {
                    for (int lang = 0; lang < 16 && !out->name[0]; lang++) {  // AmericanEnglish first, then the others
                        nacp_text(out->name, sizeof out->name, nacp + lang * 0x300, 0x200);
                        nacp_text(out->publisher, sizeof out->publisher, nacp + lang * 0x300 + 0x200, 0x100);
                    }
                    nacp_text(out->version, sizeof out->version, nacp + 0x3060, 0x10);
                }
                free(nacp);
            }
            IconPick ip = {{0}};
            nx_romtab_walk(sec_rd, &rd, pick_icon, &ip);
            if (ip.best[0] && nx_romtab_find(sec_rd, &rd, ip.best, &off, &size) && size > 0 && size < (4u << 20)) {
                out->icon = malloc(size);
                if (out->icon && sec_rd(&rd, out->icon, size, off) == (ssize_t)size) out->icon_size = (size_t)size;
                else free(out->icon), out->icon = NULL;
            }
        }
    }
    why[0] = 0;
    ok = true;
out:
    if (fd >= 0) close(fd);
    free(files.e);
    free(ks.k);
    return ok;
}

void nx_package_info_free(NxPackageInfo *i) {
    free(i->icon);
    i->icon = NULL, i->icon_size = 0;
}
