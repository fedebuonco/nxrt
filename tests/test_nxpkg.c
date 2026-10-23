// Package RomFS (runtime/nxpkg.c): a synthetic romfs.src + package with an uncompressed image, a CompressedStorage
// BucketTree (LZ4, zeros and copy blocks) and corrupt tables. nx_romfs_open is the gate for every package game; a bad
// table must be refused, not indexed.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "nxpkg.h"

#define TABLE_OFF 0
#define TABLE_SIZE 0x8000
#define DATA_OFF 0x8000
#define NODE 0x4000
#define ENTRY 0x18

static char g_dir[512];

typedef struct {
    int64_t virt, phys;
    uint8_t type;
    uint32_t psize;
} E;

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}
static void put64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

static void hex(char *out, const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static void rm_rf(const char *p) {  // (files only here)
    char c[1100];
    snprintf(c, sizeof c, "%s/game.nsp", p);
    remove(c);
    snprintf(c, sizeof c, "%s/romfs.src", p);
    remove(c);
    rmdir(p);
}

// A valid (or deliberately broken) compressed table: one node with `n` entries and the sentinel end.
static void make_table(uint8_t *table, int count, const E *e, int n, int64_t end) {
    memset(table, 0, TABLE_SIZE);
    (void)count;  // (the count lives in the BKTR header, in romfs.src)
    put32(table + NODE, 0), put32(table + NODE + 4, (uint32_t)n), put64(table + NODE + 8, (uint64_t)end);
    for (int i = 0; i < n; i++) {
        uint8_t *p = table + NODE + 0x10 + i * ENTRY;
        put64(p, (uint64_t)e[i].virt);
        put64(p + 8, (uint64_t)e[i].phys);
        p[16] = e[i].type;
        put32(p + 20, e[i].psize);
    }
}

// Writes the package and its romfs.src. table_size 0: an uncompressed image in package[data_off...].
static int write_case(const uint8_t *table, size_t table_size, const uint8_t *data, size_t data_size, size_t data_off,
                      size_t image_size, const uint8_t hdr[16]) {
    size_t pkg_size = DATA_OFF + (data_size ? data_size : 1);
    if (table_size && table_size > pkg_size) pkg_size = table_size;
    if (pkg_size < 0x100) pkg_size = 0x100;
    uint8_t *pkg = calloc(1, pkg_size);
    if (!pkg) return -1;
    if (table_size) memcpy(pkg + TABLE_OFF, table, table_size);
    if (data_size) memcpy(pkg + DATA_OFF, data, data_size);
    char pkg_path[900], src_path[900];
    snprintf(pkg_path, sizeof pkg_path, "%s/game.nsp", g_dir);
    snprintf(src_path, sizeof src_path, "%s/romfs.src", g_dir);
    FILE *f = fopen(pkg_path, "wb");
    if (!f || fwrite(pkg, 1, pkg_size, f) != pkg_size) {
        if (f) fclose(f);
        free(pkg);
        return -1;
    }
    fclose(f);
    free(pkg);
    char ctr[33], key[33], h[33];
    uint8_t zeros[16] = {0};
    hex(ctr, zeros, 16), hex(key, zeros, 16), hex(h, hdr ? hdr : zeros, 16);
    f = fopen(src_path, "w");
    if (!f) return -1;
    fprintf(f, "package=%s\nsize=%llu\nnca=0\nsection=0 %llu 1 %s\nkey=%s\ndata=%llu %llu\ncomp=%d %llu %s\n", pkg_path,
            (unsigned long long)pkg_size, (unsigned long long)pkg_size, ctr, key, (unsigned long long)data_off,
            (unsigned long long)image_size, table_size ? TABLE_OFF : 0, (unsigned long long)table_size, h);
    fclose(f);
    return 0;
}

static NxRomfs *open_case(const uint8_t *table, size_t table_size, const uint8_t *data, size_t data_size,
                          size_t data_off, size_t image_size, const uint8_t hdr[16], const char **why) {
    static char msg[256];
    if (write_case(table, table_size, data, data_size, data_off, image_size, hdr) != 0) return NULL;
    char src[900];
    snprintf(src, sizeof src, "%s/romfs.src", g_dir);
    msg[0] = 0;
    NxRomfs *r = nx_romfs_open(src, msg, sizeof msg);
    if (why) *why = msg;
    return r;
}

static void test_uncompressed(void) {
    static const uint8_t data[] = "hello";
    const char *why = NULL;
    NxRomfs *r = open_case(NULL, 0, data, sizeof data - 1, DATA_OFF, sizeof data - 1, NULL, &why);
    CHECK(r != NULL);
    if (!r) return;
    CHECK_EQ(nx_romfs_size(r), sizeof data - 1);
    uint8_t buf[8] = {0};
    CHECK_EQ(nx_romfs_pread(r, buf, 5, 0), 5);
    CHECK_MEM(buf, "hello", 5);
    CHECK_EQ(nx_romfs_pread(r, buf, 5, 3), 2);
    CHECK_MEM(buf, "lo", 2);
    CHECK_EQ(nx_romfs_pread(r, buf, 5, 5), 0);
    nx_romfs_close(r);
}

static void test_compressed(void) {
    // Two entries: an LZ4 "hello" then a copy "world", read across the boundary.
    uint8_t table[TABLE_SIZE];
    uint8_t data[0x40] = {0};
    // literal-only LZ4 block for "hello"
    static const uint8_t lz[] = {0x50, 'h', 'e', 'l', 'l', 'o'};
    memcpy(data, lz, sizeof lz);
    memcpy(data + 0x20, "world", 5);
    E e[2] = {{0, DATA_OFF, 3, (uint32_t)sizeof lz}, {5, DATA_OFF + 0x20, 0, 0}};
    make_table(table, 2, e, 2, 10);
    uint8_t hdr[16] = {0};
    memcpy(hdr, "BKTR", 4);
    put32(hdr + 8, 2);
    const char *why = NULL;
    NxRomfs *r = open_case(table, sizeof table, data, sizeof data, 0, 10, hdr, &why);
    CHECK(r != NULL);
    if (!r) {
        fprintf(stderr, "  why: %s\n", why);
        return;
    }
    CHECK_EQ(nx_romfs_size(r), 10);
    uint8_t buf[16] = {0};
    CHECK_EQ(nx_romfs_pread(r, buf, 10, 0), 10);
    CHECK_MEM(buf, "helloworld", 10);
    CHECK_EQ(nx_romfs_pread(r, buf, 4, 3), 4);
    CHECK_MEM(buf, "lowo", 4);
    CHECK_EQ(nx_romfs_pread(r, buf, 5, 10), 0);
    // The cache serves the second read of the same LZ4 block.
    CHECK_EQ(nx_romfs_pread(r, buf, 5, 0), 5);
    CHECK_MEM(buf, "hello", 5);
    nx_romfs_close(r);
}

static void test_zeros(void) {
    uint8_t table[TABLE_SIZE];
    E e = {0, 0, 1, 0};
    make_table(table, 1, &e, 1, 16);
    uint8_t hdr[16] = {0};
    memcpy(hdr, "BKTR", 4);
    put32(hdr + 8, 1);
    NxRomfs *r = open_case(table, sizeof table, NULL, 0, 0, 16, hdr, NULL);
    CHECK(r != NULL);
    if (!r) return;
    uint8_t buf[16];
    memset(buf, 0xAA, sizeof buf);
    CHECK_EQ(nx_romfs_pread(r, buf, 16, 0), 16);
    for (int i = 0; i < 16; i++) CHECK_EQ(buf[i], 0);
    nx_romfs_close(r);
}

static void expect_bad(const char *what, int count, const E *e, int n, int64_t end, size_t table_size, size_t image,
                       const uint8_t hdr_override[16]) {
    uint8_t table[TABLE_SIZE];
    make_table(table, count, e, n, end);
    uint8_t hdr[16] = {0};
    memcpy(hdr, "BKTR", 4);
    put32(hdr + 8, (uint32_t)count);
    if (hdr_override) memcpy(hdr, hdr_override, 16);
    const char *why = NULL;
    NxRomfs *r = open_case(table, table_size, NULL, 0, 0, image, hdr, &why);
    t_checks++;
    if (r) {
        t_failures++;
        fprintf(stderr, "FAIL: %s was accepted\n", what);
        nx_romfs_close(r);
    } else if (!why || !why[0]) {
        t_failures++;
        fprintf(stderr, "FAIL: %s was refused without a reason\n", what);
    }
}

static void test_corrupt(void) {
    E good = {0, DATA_OFF, 1, 0};
    static const uint8_t bad_magic[16] = {'X', 'X', 'X', 'X'};
    expect_bad("bad magic", 1, &good, 1, 16, TABLE_SIZE, 16, bad_magic);
    expect_bad("count 0", 0, NULL, 0, 16, TABLE_SIZE, 16, NULL);
    expect_bad("negative count", -1, NULL, 0, 16, TABLE_SIZE, 16, NULL);
    expect_bad("count larger than the table", 0x40000000, &good, 1, 16, TABLE_SIZE, 16, NULL);
    // First entry not at 0: nx_romfs_pread's binary search assumes it.
    E off0 = {5, DATA_OFF, 1, 0};
    expect_bad("first entry not at 0", 1, &off0, 1, 16, TABLE_SIZE, 16, NULL);
    // Entries out of order.
    E unordered[2] = {{5, DATA_OFF, 1, 0}, {3, DATA_OFF, 1, 0}};
    expect_bad("decreasing virt", 2, unordered, 2, 16, TABLE_SIZE, 16, NULL);
    // The sentinel ends before the last entry.
    E last = {10, DATA_OFF, 1, 0};
    expect_bad("end before the last entry", 1, &last, 1, 5, TABLE_SIZE, 16, NULL);
    // An unknown block type.
    E nasty = {0, DATA_OFF, 2, 0};
    expect_bad("unknown block type", 1, &nasty, 1, 16, TABLE_SIZE, 16, NULL);
    // A physical size beyond the section.
    E huge = {0, DATA_OFF, 3, 0xFFFFFFFFu};
    expect_bad("oversized physical block", 1, &huge, 1, 16, TABLE_SIZE, 16, NULL);
    // A table cut short before its node.
    expect_bad("truncated table", 1, &good, 1, 16, 0x2000, 16, NULL);
}

static void test_package_moved(void) {
    static const uint8_t data[] = "hello";
    write_case(NULL, 0, data, sizeof data - 1, DATA_OFF, sizeof data - 1, NULL);
    char src[900], pkg[900];
    snprintf(src, sizeof src, "%s/romfs.src", g_dir);
    snprintf(pkg, sizeof pkg, "%s/game.nsp", g_dir);
    char why[256];
    // The package changed size since setup: refused.
    FILE *f = fopen(pkg, "ab");
    if (f) {
        fputc('x', f);
        fclose(f);
    }
    NxRomfs *r = nx_romfs_open(src, why, sizeof why);
    CHECK(r == NULL);
    CHECK(strstr(why, "moved or changed") != NULL);
    if (r) nx_romfs_close(r);
    // Missing package: refused.
    remove(pkg);
    r = nx_romfs_open(src, why, sizeof why);
    CHECK(r == NULL);
    if (r) nx_romfs_close(r);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/nxrt-nxpkg-XXXXXX");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 2;
    }
    test_uncompressed();
    test_compressed();
    test_zeros();
    test_corrupt();
    test_package_moved();
    rm_rf(g_dir);
    TEST_END();
}
