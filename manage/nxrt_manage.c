// libnxrt_manage (nxrt_manage.h): a game folder's files for a manager app, on nxpkg.c's package reader.
#include "nxrt_manage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nxpkg.h"

int nxm_abi(void) { return NXM_ABI; }

static void say(char *why, size_t n, const char *fmt, const char *arg) {
    if (why && n) snprintf(why, n, fmt, arg ? arg : "");
}

static char *join(const char *a, const char *b) {
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", a, b);
    return p;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

bool nxm_nso_build_id(const char *path, uint8_t out[8]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[0x48];
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4);
    fclose(f);
    if (ok) memcpy(out, hdr + 0x40, 8);
    return ok;
}

bool nxm_game_files(const char *game_dir, NxmGameFiles *out) {
    memset(out, 0, sizeof *out);
    if (!game_dir || !is_dir(game_dir)) return false;
    char *main = join(game_dir, "exefs/main"), *rd = join(game_dir, "romfs"), *rs = join(game_dir, "romfs.src");
    if (main && rd && rs) {
        out->exefs = nxm_nso_build_id(main, out->build_id);
        out->romfs_dir = is_dir(rd);
        out->romfs_package = is_file(rs);
    }
    free(main), free(rd), free(rs);
    return true;
}

bool nxm_package_setup(const char *package, const char *keys, const char *out_dir, uint64_t *title_id, char *why,
                       size_t why_size) {
    return nx_setup(package, keys, out_dir, title_id, why, why_size);
}

bool nxm_package_info(const char *package, const char *keys, NxmPackage *out, char *why, size_t why_size) {
    NxPackageInfo i;
    memset(out, 0, sizeof *out);
    if (!nx_package_info(package, keys, &i, why, why_size)) return false;
    out->title_id = i.title_id;
    memcpy(out->build_id, i.build_id, 8);
    out->has_build_id = i.has_build_id;
    memcpy(out->name, i.name, sizeof out->name);
    memcpy(out->publisher, i.publisher, sizeof out->publisher);
    memcpy(out->version, i.version, sizeof out->version);
    out->icon = i.icon, out->icon_size = i.icon_size;  // (handed over)
    return true;
}

void nxm_package_free(NxmPackage *p) {
    free(p->icon);
    p->icon = NULL, p->icon_size = 0;
}

bool nxm_find_keys(const char *package, char *out, size_t out_size) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", package);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    else snprintf(dir, sizeof dir, ".");
    snprintf(out, out_size, "%s/prod.keys", dir);
    if (is_file(out)) return true;
    const char *home = getenv("HOME");
    if (home) {
        snprintf(out, out_size, "%s/.switch/prod.keys", home);
        if (is_file(out)) return true;
    }
    out[0] = 0;
    return false;
}

// ------------------------------------------------------------------ RomFS: a folder, or the package read in place
struct NxmRomfs {
    char *dir;    // romfs/ (a folder game), or NULL
    NxRomfs *pkg; // romfs.src's package, or NULL
};

NxmRomfs *nxm_romfs_open(const char *game_dir, char *why, size_t why_size) {
    NxmRomfs *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    char *rd = join(game_dir, "romfs"), *rs = join(game_dir, "romfs.src");
    if (rd && is_dir(rd)) r->dir = rd, rd = NULL;
    else if (rs && is_file(rs)) r->pkg = nx_romfs_open(rs, why, why_size);
    else say(why, why_size, "no game data in %s (romfs/ or romfs.src)", game_dir);
    free(rd), free(rs);
    if (!r->dir && !r->pkg) { free(r); return NULL; }
    return r;
}

void nxm_romfs_close(NxmRomfs *r) {
    if (!r) return;
    if (r->pkg) nx_romfs_close(r->pkg);
    free(r->dir);
    free(r);
}

// (a folder: paths stay inside it)
static bool safe_rel(const char *p) {
    if (!p || !*p || *p == '/') return false;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if ((n == 2 && !memcmp(s, "..", 2)) || n == 0) return false;
        if (!e) break;
        s = e + 1;
    }
    return true;
}

typedef struct { void (*fn)(const char *, uint64_t, void *); void *ctx; } ListCtx;
static void list_pkg(const char *path, uint64_t off, uint64_t size, void *c) {
    (void)off;
    ListCtx *l = c;
    l->fn(path, size, l->ctx);
}

static int list_dir(const char *root, char *rel, size_t at, size_t cap, int depth, ListCtx *l) {
    char *abs = at ? join(root, rel) : strdup(root);
    if (!abs) return -1;
    DIR *d = opendir(abs);
    if (!d) { free(abs); return -1; }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        if (!strncmp(e->d_name, "._", 2)) continue;  // (macOS resource forks on exFAT drives)
        size_t n = strlen(e->d_name);
        if (at + n + 2 > cap) continue;
        memcpy(rel + at, e->d_name, n + 1);
        char *p = join(abs, e->d_name);
        struct stat st;
        if (p && stat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode) && depth < 64) {
                rel[at + n] = '/', rel[at + n + 1] = 0;
                int k = list_dir(root, rel, at + n + 1, cap, depth + 1, l);
                if (k > 0) count += k;
            } else if (S_ISREG(st.st_mode)) {
                l->fn(rel, (uint64_t)st.st_size, l->ctx);
                count++;
            }
        }
        free(p);
        rel[at] = 0;
    }
    closedir(d);
    free(abs);
    return count;
}

int nxm_romfs_list(NxmRomfs *r, void (*fn)(const char *path, uint64_t size, void *ctx), void *ctx) {
    ListCtx l = {fn, ctx};
    if (r->pkg) return nx_romfs_walk(r->pkg, list_pkg, &l);
    char rel[1024] = "";
    return list_dir(r->dir, rel, 0, sizeof rel, 0, &l);
}

int64_t nxm_romfs_file_size(NxmRomfs *r, const char *path) {
    if (r->pkg) {
        uint64_t off, size;
        return nx_romfs_find(r->pkg, path, &off, &size) ? (int64_t)size : -1;
    }
    if (!safe_rel(path)) return -1;
    char *p = join(r->dir, path);
    struct stat st;
    int64_t n = p && stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (int64_t)st.st_size : -1;
    free(p);
    return n;
}

int64_t nxm_romfs_read(NxmRomfs *r, const char *path, uint64_t off, void *dst, uint64_t n) {
    if (r->pkg) {
        uint64_t at, size;
        if (!nx_romfs_find(r->pkg, path, &at, &size)) return -1;
        if (off >= size) return 0;
        if (n > size - off) n = size - off;
        return nx_romfs_pread(r->pkg, dst, n, at + off);
    }
    if (!safe_rel(path)) return -1;
    char *p = join(r->dir, path);
    int fd = p ? open(p, O_RDONLY) : -1;
    free(p);
    if (fd < 0) return -1;
    ssize_t got = pread(fd, dst, n, (off_t)off);
    close(fd);
    return got;
}

bool nxm_romfs_extract(NxmRomfs *r, const char *path, const char *out_path, char *why, size_t why_size) {
    int64_t size = nxm_romfs_file_size(r, path);
    if (size < 0) { say(why, why_size, "%s: not in the game data", path); return false; }
    FILE *f = fopen(out_path, "wb");
    if (!f) { say(why, why_size, "can't write %s", out_path); return false; }
    enum { CHUNK = 1 << 20 };
    uint8_t *b = malloc(CHUNK);
    bool ok = b != NULL;
    for (int64_t at = 0; ok && at < size;) {
        int64_t got = nxm_romfs_read(r, path, (uint64_t)at, b, CHUNK);
        ok = got > 0 && fwrite(b, 1, (size_t)got, f) == (size_t)got;
        at += got;
    }
    free(b);
    if (fclose(f) != 0) ok = false;
    if (!ok) { say(why, why_size, "%s: read or write failed", path); unlink(out_path); }
    return ok;
}
