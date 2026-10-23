// RomFS image layout (runtime/romfs_build.c): build the layout of a temporary tree, then read it back with an
// independent implementation of the RomFS format (header, hash buckets, directory/file entries, name hash). This is
// what fs.c serves to the game, so a wrong offset here is a game that can't find its data.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "romfs_build.h"

#define EMPTY 0xFFFFFFFFu

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

// The game's RomFS name hash and bucket rule (Switchbrew / Atmosphere's fssystem).
static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = (h >> 5) | (h << 27), h ^= *p;
    return h;
}

typedef struct {
    const uint8_t *hdr, *tables;
    uint64_t nbd, nbf, dmeta, dsz, fmeta, fsz, data;
} Image;

static void image_init(Image *im, const RomfsLayout *l) {
    im->hdr = l->header;
    im->tables = l->tables;
    im->nbd = rd32(l->header + 2 * 8) / 4;
    im->dmeta = rd32(l->header + 3 * 8);
    im->dsz = rd32(l->header + 4 * 8);
    im->fmeta = rd32(l->header + 7 * 8);
    im->fsz = rd32(l->header + 8 * 8);
    im->data = rd32(l->header + 9 * 8);
    im->nbf = rd32(l->header + 6 * 8) / 4;
    im->dmeta -= l->tables_off;
    im->fmeta -= l->tables_off;
    im->data -= 0;  // (an image offset, not a table offset)
}

// Directory entry: parent, sibling, first dir, first file, hash link, name length, name.
static const uint8_t *dir_at(const Image *im, uint32_t off) {
    return im->tables + im->dmeta + off;
}
static uint32_t dir_parent(const uint8_t *e) {
    return rd32(e);
}
static uint32_t dir_sibling(const uint8_t *e) {
    return rd32(e + 4);
}
static uint32_t dir_first_dir(const uint8_t *e) {
    return rd32(e + 8);
}
static uint32_t dir_first_file(const uint8_t *e) {
    return rd32(e + 12);
}
static uint32_t dir_chain(const uint8_t *e) {
    return rd32(e + 16);
}
static uint32_t dir_name_len(const uint8_t *e) {
    return rd32(e + 20);
}
static const char *dir_name(const uint8_t *e) {
    return (const char *)e + 0x18;
}

// File entry: parent, sibling, data offset, size, hash link, name length, name.
static const uint8_t *file_at(const Image *im, uint32_t off) {
    return im->tables + im->fmeta + off;
}
static uint32_t file_parent(const uint8_t *e) {
    return rd32(e);
}
static uint32_t file_sibling(const uint8_t *e) {
    return rd32(e + 4);
}
static uint64_t file_data(const uint8_t *e) {
    return rd64(e + 8);
}
static uint64_t file_size(const uint8_t *e) {
    return rd64(e + 16);
}
static uint32_t file_chain(const uint8_t *e) {
    return rd32(e + 24);
}
static uint32_t file_name_len(const uint8_t *e) {
    return rd32(e + 28);
}
static const char *file_name(const uint8_t *e) {
    return (const char *)e + 0x20;
}

static uint32_t find_bucket(const Image *im, const uint32_t *buckets, uint32_t nb, uint32_t parent, const char *name,
                            int file) {
    uint32_t bucket = name_hash(parent, name) % nb, off = buckets[bucket], guard = 0;
    while (off != EMPTY && guard++ < 100000) {
        const uint8_t *e = file ? file_at(im, off) : dir_at(im, off);
        uint32_t nl = file ? file_name_len(e) : dir_name_len(e);
        const char *n = file ? file_name(e) : dir_name(e);
        if (nl == strlen(name) && !memcmp(n, name, nl)) return off;
        off = file ? file_chain(e) : dir_chain(e);
    }
    return EMPTY;
}

// The path a dir/file entry's parent chain spells.
static void path_of(const Image *im, uint32_t parent, const char *name, char *out, size_t cap) {
    char parts[32][256];
    int n = 0;
    while (parent != 0 && parent < im->dsz && n < 32) {
        const uint8_t *e = dir_at(im, parent);
        uint32_t nl = dir_name_len(e);
        if (nl > 255) break;
        memcpy(parts[n], dir_name(e), nl);
        parts[n][nl] = 0;
        n++;
        parent = dir_parent(e);
    }
    out[0] = 0;
    for (int i = n - 1; i >= 0; i--) {
        strncat(out, parts[i], cap - strlen(out) - 1);
        strncat(out, "/", cap - strlen(out) - 1);
    }
    strncat(out, name, cap - strlen(out) - 1);
}

// Read the image as fs.c serves it: header, tables, then each file's bytes from its path (gaps zero).
static int image_read(const RomfsLayout *l, uint64_t off, void *dst_, size_t n) {
    uint8_t *dst = dst_;
    memset(dst, 0, n);
    for (size_t i = 0; i < n; i++) {
        uint64_t o = off + i;
        if (o < ROMFS_DATA_START) {
            if (o < 0x50) dst[i] = l->header[o];
        } else if (o >= l->tables_off && o < l->tables_off + l->tables_size) {
            dst[i] = l->tables[o - l->tables_off];
        } else {
            for (int f = 0; f < l->nfiles; f++) {
                const RomfsLayoutFile *rf = &l->files[f];
                if (o >= rf->start && o < rf->start + rf->size) {
                    FILE *fh = fopen(rf->path, "rb");
                    if (!fh) return -1;
                    if (fseek(fh, (long)(o - rf->start), SEEK_SET) != 0) {
                        fclose(fh);
                        return -1;
                    }
                    int c = fgetc(fh);
                    fclose(fh);
                    if (c == EOF) return -1;
                    dst[i] = (uint8_t)c;
                    break;
                }
            }
        }
    }
    return 0;
}

// ---- temporary tree
static char g_tmp[512];

static void mkdir_p(const char *p) {
    char b[1024];
    snprintf(b, sizeof b, "%s", p);
    for (char *c = b + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(b, 0755);
            *c = '/';
        }
    mkdir(b, 0755);
}

static void write_file(const char *dir, const char *name, const uint8_t *data, size_t n) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) {
        perror(p);
        exit(2);
    }
    if (n && fwrite(data, 1, n, f) != n) {
        perror(p);
        exit(2);
    }
    fclose(f);
}

static void rm_rf(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char c[1200];
                snprintf(c, sizeof c, "%s/%s", p, e->d_name);
                rm_rf(c);
            }
            closedir(d);
        }
        rmdir(p);
    } else {
        remove(p);
    }
}

static void make_tree(void) {
    snprintf(g_tmp, sizeof g_tmp, "/tmp/nxrt-romfs-XXXXXX");
    if (!mkdtemp(g_tmp)) {
        perror("mkdtemp");
        exit(2);
    }
    char p[1200];
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(i * 37 + 11);
    snprintf(p, sizeof p, "%s/a.txt", g_tmp);
    write_file(g_tmp, "a.txt", buf, 5);
    write_file(g_tmp, "z.bin", buf, 0);
    write_file(g_tmp, "space name.txt", buf + 5, 3);
    mkdir_p((snprintf(p, sizeof p, "%s/empty_dir", g_tmp), p));
    mkdir_p((snprintf(p, sizeof p, "%s/sub", g_tmp), p));
    mkdir_p((snprintf(p, sizeof p, "%s/sub/deeper", g_tmp), p));
    write_file((snprintf(p, sizeof p, "%s/sub", g_tmp), p), "b.bin", buf, 16);
    write_file((snprintf(p, sizeof p, "%s/sub/deeper", g_tmp), p), "c.bin", buf, 17);
    // A file whose name is not ASCII: RomFS names are bytes.
    write_file(g_tmp, "caf\xc3\xa9.bin", buf, 9);
}

static void test_layout(void) {
    make_tree();
    RomfsLayout l;
    char why[256] = "";
    CHECK(romfs_layout(g_tmp, &l, why, sizeof why));
    if (!l.tables) {
        fprintf(stderr, "layout failed: %s\n", why);
        rm_rf(g_tmp);
        return;
    }
    Image im;
    image_init(&im, &l);

    // ---- header
    CHECK_EQ(rd64(l.header), 0x50);  // header size
    CHECK_EQ(rd64(l.header + 9 * 8), ROMFS_DATA_START);
    CHECK_EQ(l.tables_off, rd64(l.header + 1 * 8));
    CHECK_EQ(l.tables_size, rd64(l.header + 7 * 8) + rd64(l.header + 8 * 8) - l.tables_off);
    CHECK_EQ(l.size, l.tables_off + l.tables_size);
    CHECK(l.tables_off >= ROMFS_DATA_START);
    CHECK_EQ(l.tables_off % 4, 0);
    // The four tables are contiguous: dir hash, dir entries, file hash, file entries.
    CHECK_EQ(rd64(l.header + 3 * 8), rd64(l.header + 1 * 8) + rd64(l.header + 2 * 8));
    CHECK_EQ(rd64(l.header + 5 * 8), rd64(l.header + 3 * 8) + rd64(l.header + 4 * 8));
    CHECK_EQ(rd64(l.header + 7 * 8), rd64(l.header + 5 * 8) + rd64(l.header + 6 * 8));
    CHECK_EQ(im.data, ROMFS_DATA_START);

    // ---- every entry is inside its table and its links are in range
    for (uint64_t o = 0; o < im.dsz;) {
        const uint8_t *e = dir_at(&im, (uint32_t)o);
        uint32_t nl = dir_name_len(e);
        CHECK(o + 0x18 + nl <= im.dsz);
        CHECK(dir_parent(e) == 0 || dir_parent(e) < im.dsz);
        CHECK(dir_sibling(e) == EMPTY || dir_sibling(e) < im.dsz);
        CHECK(dir_chain(e) == EMPTY || dir_chain(e) < im.dsz);
        CHECK(dir_first_dir(e) == EMPTY || dir_first_dir(e) < im.dsz);
        CHECK(dir_first_file(e) == EMPTY || dir_first_file(e) < im.fsz);
        for (uint32_t i = 0; i < nl; i++) CHECK(dir_name(e)[i] != '/' && dir_name(e)[i] != 0);
        o += 0x18 + ((nl + 3) & ~3u);
    }
    for (uint64_t o = 0; o < im.fsz;) {
        const uint8_t *e = file_at(&im, (uint32_t)o);
        uint32_t nl = file_name_len(e);
        CHECK(o + 0x20 + nl <= im.fsz);
        CHECK(file_parent(e) < im.dsz);
        CHECK(file_sibling(e) == EMPTY || file_sibling(e) < im.fsz);
        CHECK(file_chain(e) == EMPTY || file_chain(e) < im.fsz);
        CHECK_EQ(file_data(e) % 16, 0);
        CHECK(ROMFS_DATA_START + file_data(e) + file_size(e) <= l.tables_off);
        for (uint32_t i = 0; i < nl; i++) CHECK(file_name(e)[i] != '/' && file_name(e)[i] != 0);
        o += 0x20 + ((nl + 3) & ~3u);
    }

    // ---- each file's entry is reachable by its path through the hash tables
    for (int i = 0; i < l.nfiles; i++) {
        const RomfsLayoutFile *rf = &l.files[i];
        char rel[1024], parent_path[1024];
        // Strip the temp root; the rest is the RomFS path.
        snprintf(rel, sizeof rel, "%s", rf->path + strlen(g_tmp) + 1);
        char *slash = strrchr(rel, '/');
        const char *base = slash ? slash + 1 : rel;
        if (slash) {
            *slash = 0;
            snprintf(parent_path, sizeof parent_path, "%s", rel);
        } else
            parent_path[0] = 0;
        // Walk the directory components.
        uint32_t dir = 0;
        char walk[1024];
        snprintf(walk, sizeof walk, "%s", parent_path);
        for (char *tok = strtok(walk, "/"); tok; tok = strtok(NULL, "/")) {
            const uint32_t *db = (const uint32_t *)(l.tables + rd64(l.header + 1 * 8) - l.tables_off);
            uint32_t off = find_bucket(&im, db, (uint32_t)im.nbd, dir, tok, 0);
            CHECK(off != EMPTY);
            if (off == EMPTY) break;
            dir = off;
        }
        const uint32_t *fb = (const uint32_t *)(l.tables + rd64(l.header + 5 * 8) - l.tables_off);
        uint32_t foff = find_bucket(&im, fb, (uint32_t)im.nbf, dir, base, 1);
        CHECK(foff != EMPTY);
        if (foff == EMPTY) continue;
        const uint8_t *e = file_at(&im, foff);
        CHECK_EQ(file_data(e), rf->start - ROMFS_DATA_START);
        CHECK_EQ(file_size(e), rf->size);
        // The path the entry's parent chain spells is the same path.
        char got[1024];
        path_of(&im, file_parent(e), file_name(e), got, sizeof got);
        CHECK_STR(got, rf->path + strlen(g_tmp) + 1);
        // And the bytes served at its start are the file's.
        if (rf->size) {
            uint8_t *a = malloc((size_t)rf->size), *b = malloc((size_t)rf->size);
            FILE *f = fopen(rf->path, "rb");
            CHECK(f && fread(b, 1, (size_t)rf->size, f) == rf->size);
            if (f) fclose(f);
            CHECK_EQ(image_read(&l, rf->start, a, (size_t)rf->size), 0);
            CHECK_MEM(a, b, (size_t)rf->size);
            free(a);
            free(b);
        }
    }

    // ---- entries do not overlap, are 16-byte aligned, and cover the tree's bytes
    uint64_t sum = 0, prev_end = ROMFS_DATA_START;
    for (int i = 0; i < l.nfiles; i++) {
        const RomfsLayoutFile *rf = &l.files[i];
        CHECK(rf->start >= prev_end);
        CHECK(rf->start + rf->size <= l.tables_off);
        CHECK(rf->start % 16 == 0);
        prev_end = rf->start + rf->size;
        sum += rf->size;
    }
    CHECK_EQ(sum, romfs_dir_bytes(g_tmp));
    // Every file in the tree is in the layout.
    CHECK_EQ(l.nfiles, 6);

    // ---- the full image reads back consistently, including the tables
    uint8_t *img = malloc((size_t)l.size);
    CHECK_EQ(image_read(&l, 0, img, (size_t)l.size), 0);
    CHECK_MEM(img, l.header, 0x50);
    CHECK_MEM(img + l.tables_off, l.tables, (size_t)l.tables_size);
    free(img);

    romfs_layout_free(&l);
    rm_rf(g_tmp);
}

static void test_errors(void) {
    RomfsLayout l;
    char why[128] = "";
    CHECK(!romfs_layout("/nonexistent/nxrt-romfs", &l, why, sizeof why));
    CHECK(why[0] != 0);

    char t[] = "/tmp/nxrt-romfs-empty-XXXXXX";
    if (mkdtemp(t)) {
        why[0] = 0;
        CHECK(!romfs_layout(t, &l, why, sizeof why));
        CHECK(why[0] != 0);  // "no files in ..."
        rmdir(t);
    }
}

int main(void) {
    test_layout();
    test_errors();
    TEST_END();
}
