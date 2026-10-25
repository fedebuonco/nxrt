// libnxrt_manage (manage/nxrt_manage.c) and the RomFS path lookup under it (runtime/nxpkg.c nx_romtab_find / walk):
// a folder game read through nxm_*, and a RomFS image (laid out by runtime/romfs_build.c, as fs.c serves it) looked up
// by path the way a package's RomFS is - the start screen's title movie and a manager's file list depend on both.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "nxpkg.h"
#include "nxrt_manage.h"
#include "romfs_build.h"

static char g_tmp[512];

static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void rm_rf(const char *p) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd) != 0) fprintf(stderr, "cleanup of %s failed\n", p);
}

static char *pathf(const char *rel) {
    static char b[4][1200];
    static int k;
    char *p = b[k++ & 3];
    snprintf(p, 1200, "%s/%s", g_tmp, rel);
    return p;
}

// ---- a game folder: exefs/main (an NSO header), romfs/ with a nested file
static uint8_t g_data[300];
static void make_game(void) {
    mkdir(pathf("game"), 0755), mkdir(pathf("game/exefs"), 0755), mkdir(pathf("game/romfs"), 0755);
    mkdir(pathf("game/romfs/nx"), 0755), mkdir(pathf("game/romfs/nx/movie"), 0755);
    uint8_t nso[0x100] = {'N', 'S', 'O', '0'};
    for (int i = 0; i < 8; i++) nso[0x40 + i] = (uint8_t)(0xA0 + i);
    write_file(pathf("game/exefs/main"), nso, sizeof nso);
    for (size_t i = 0; i < sizeof g_data; i++) g_data[i] = (uint8_t)(i * 7 + 3);
    write_file(pathf("game/romfs/nx/movie/op.webm"), g_data, sizeof g_data);
    write_file(pathf("game/romfs/top.bin"), g_data, 10);
    write_file(pathf("game/romfs/._top.bin"), g_data, 4);  // (an exFAT resource fork: not a game file)
}

typedef struct { int n; uint64_t movie_size; bool saw_fork; } Seen;
static void seen(const char *path, uint64_t size, void *c) {
    Seen *s = c;
    s->n++;
    if (!strcmp(path, "nx/movie/op.webm")) s->movie_size = size;
    if (strstr(path, "._")) s->saw_fork = true;
}

static void test_folder_game(void) {
    CHECK_EQ(nxm_abi(), NXM_ABI);
    NxmGameFiles g;
    CHECK(!nxm_game_files(pathf("missing"), &g));
    CHECK(nxm_game_files(pathf("game"), &g));
    CHECK(g.exefs && g.romfs_dir && !g.romfs_package);
    CHECK_EQ(g.build_id[0], 0xA0);
    CHECK_EQ(g.build_id[7], 0xA7);

    char why[256] = "";
    CHECK(nxm_romfs_open(pathf("game/exefs"), why, sizeof why) == NULL);  // (no romfs there)
    CHECK(why[0] != 0);
    NxmRomfs *r = nxm_romfs_open(pathf("game"), why, sizeof why);
    CHECK(r != NULL);
    if (!r) return;
    Seen s = {0};
    CHECK_EQ(nxm_romfs_list(r, seen, &s), 2);
    CHECK_EQ(s.n, 2);
    CHECK_EQ(s.movie_size, sizeof g_data);
    CHECK(!s.saw_fork);
    CHECK_EQ(nxm_romfs_file_size(r, "nx/movie/op.webm"), sizeof g_data);
    CHECK_EQ(nxm_romfs_file_size(r, "nx/movie/none.webm"), -1);
    CHECK_EQ(nxm_romfs_file_size(r, "../exefs/main"), -1);  // (stays inside romfs/)
    uint8_t b[64];
    CHECK_EQ(nxm_romfs_read(r, "nx/movie/op.webm", 290, b, sizeof b), 10);
    CHECK(!memcmp(b, g_data + 290, 10));
    CHECK_EQ(nxm_romfs_read(r, "nx/movie/op.webm", 400, b, sizeof b), 0);
    CHECK(nxm_romfs_extract(r, "nx/movie/op.webm", pathf("out.webm"), why, sizeof why));
    FILE *f = fopen(pathf("out.webm"), "rb");
    uint8_t back[sizeof g_data + 1];
    size_t n = f ? fread(back, 1, sizeof back, f) : 0;
    if (f) fclose(f);
    CHECK_EQ(n, sizeof g_data);
    CHECK(!memcmp(back, g_data, sizeof g_data));
    CHECK(!nxm_romfs_extract(r, "nope", pathf("out2"), why, sizeof why));
    nxm_romfs_close(r);
}

// ---- the same tree as a RomFS image, looked up by path (a package's RomFS goes through this)
static const RomfsLayout *g_l;
static ssize_t image_read(void *ctx, void *dst_, uint64_t n, uint64_t off) {
    (void)ctx;
    uint8_t *dst = dst_;
    memset(dst, 0, n);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t o = off + i;
        if (o < 0x50) dst[i] = g_l->header[o];
        else if (o >= g_l->tables_off && o < g_l->tables_off + g_l->tables_size) dst[i] = g_l->tables[o - g_l->tables_off];
        else
            for (int k = 0; k < g_l->nfiles; k++)
                if (o >= g_l->files[k].start && o < g_l->files[k].start + g_l->files[k].size) {
                    FILE *f = fopen(g_l->files[k].path, "rb");
                    if (!f) return -1;
                    fseek(f, (long)(o - g_l->files[k].start), SEEK_SET);
                    int c = fgetc(f);
                    fclose(f);
                    dst[i] = (uint8_t)c;
                }
    }
    return (ssize_t)n;
}

typedef struct { int n; bool movie_ok; } Walk;
static void walked(const char *path, uint64_t off, uint64_t size, void *c) {
    Walk *w = c;
    w->n++;
    if (!strcmp(path, "nx/movie/op.webm") && size == sizeof g_data) {
        uint8_t b[16];
        image_read(NULL, b, sizeof b, off + 100);
        w->movie_ok = !memcmp(b, g_data + 100, sizeof b);
    }
}

static void test_image_lookup(void) {
    RomfsLayout l;
    char why[256] = "";
    unlink(pathf("game/romfs/._top.bin"));
    CHECK(romfs_layout(pathf("game/romfs"), &l, why, sizeof why));
    if (!l.tables) return;
    g_l = &l;
    uint64_t off = 0, size = 0;
    CHECK(nx_romtab_find(image_read, NULL, "nx/movie/op.webm", &off, &size));
    CHECK_EQ(size, sizeof g_data);
    uint8_t b[32];
    image_read(NULL, b, sizeof b, off);
    CHECK(!memcmp(b, g_data, sizeof b));
    CHECK(nx_romtab_find(image_read, NULL, "top.bin", &off, &size));
    CHECK_EQ(size, 10);
    CHECK(!nx_romtab_find(image_read, NULL, "nx/movie", &off, &size));      // (a directory, not a file)
    CHECK(!nx_romtab_find(image_read, NULL, "nx/movie/op.web", &off, &size));
    CHECK(!nx_romtab_find(image_read, NULL, "nope/op.webm", &off, &size));
    CHECK(!nx_romtab_find(image_read, NULL, "", &off, &size));
    Walk w = {0};
    CHECK_EQ(nx_romtab_walk(image_read, NULL, walked, &w), 2);
    CHECK_EQ(w.n, 2);
    CHECK(w.movie_ok);
    romfs_layout_free(&l);
}

int main(void) {
    snprintf(g_tmp, sizeof g_tmp, "%s/nxrt_test_manage_%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    rm_rf(g_tmp);
    mkdir(g_tmp, 0755);
    make_game();
    test_folder_game();
    test_image_lookup();
    rm_rf(g_tmp);
    printf("test_manage: %d checks, %d failures\n", t_checks, t_failures);
    return t_failures != 0;
}
