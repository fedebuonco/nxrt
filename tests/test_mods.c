// LayeredFS mods (runtime/mods.c): scanning mods/, disabled_mods and mod_order from the settings, priority order,
// enabling/disabling/moving, and the RomFS override lookup (the first enabled mod with the file wins).
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "mods.h"

static char g_tmp[512];
static char g_old[1024];

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

static void write_file(const char *p, const char *s) {
    FILE *f = fopen(p, "wb");
    if (!f) {
        perror(p);
        exit(2);
    }
    fputs(s, f);
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

static int index_of(const char *name) {
    for (int i = 0; i < mods_count(); i++) {
        ModEntry *m = mods_get(i);
        if (m && !strcmp(m->name, name)) return i;
    }
    return -1;
}

static void make_mods(void) {
    char p[1024];
    mkdir_p((snprintf(p, sizeof p, "%s/mods/Alpha/romfs", g_tmp), p));
    write_file((snprintf(p, sizeof p, "%s/mods/Alpha/romfs/a.txt", g_tmp), p), "alpha");
    write_file((snprintf(p, sizeof p, "%s/mods/Alpha/romfs/same.txt", g_tmp), p), "alpha-same");
    write_file((snprintf(p, sizeof p, "%s/mods/Alpha/outside.txt", g_tmp), p), "outside");
    mkdir_p((snprintf(p, sizeof p, "%s/mods/Beta/romfs", g_tmp), p));
    write_file((snprintf(p, sizeof p, "%s/mods/Beta/romfs/b.txt", g_tmp), p), "beta");
    write_file((snprintf(p, sizeof p, "%s/mods/Beta/romfs/same.txt", g_tmp), p), "beta-same");
    mkdir_p((snprintf(p, sizeof p, "%s/mods/Gamma", g_tmp), p));
    write_file((snprintf(p, sizeof p, "%s/mods/Gamma/romfs_metadata.bin", g_tmp), p), "meta");
    write_file((snprintf(p, sizeof p, "%s/mods/loose.txt", g_tmp), p), "not a mod");
}

static void test_scan(void) {
    mods_rescan();
    CHECK_EQ(mods_count(), 3);  // (loose.txt is not a folder)
    int a = index_of("Alpha"), b = index_of("Beta"), g = index_of("Gamma");
    CHECK(a >= 0 && b >= 0 && g >= 0);
    if (a < 0 || b < 0 || g < 0) return;
    CHECK_STR(mods_get(a)->romfs_path, "mods/Alpha/romfs");
    CHECK_EQ(mods_get(a)->file_count, 2);
    CHECK(!mods_get(a)->has_metadata);
    CHECK_STR(mods_get(b)->romfs_path, "mods/Beta/romfs");
    CHECK_EQ(mods_get(b)->file_count, 2);
    CHECK_STR(mods_get(g)->romfs_path, "mods/Gamma");  // no romfs/: the folder itself
    CHECK(mods_get(g)->has_metadata);
    CHECK_EQ(mods_get(g)->file_count, 0);  // (romfs_metadata.bin isn't a game file)
    CHECK(mods_get(a)->enabled);
    CHECK(mods_get(-1) == NULL && mods_get(mods_count()) == NULL);

    // The override searches mods in list order and honours enabled.
    mods_set_enabled(a, true);
    mods_set_enabled(b, false);
    mods_set_enabled(g, false);
    char host[1200];
    CHECK(mods_romfs_override("a.txt", host, sizeof host));
    CHECK_STR(host, "mods/Alpha/romfs/a.txt");
    CHECK(!mods_romfs_override("b.txt", host, sizeof host));
    CHECK(!mods_romfs_override("missing.txt", host, sizeof host));
    CHECK(!mods_romfs_override("../etc/passwd", host, sizeof host));
    CHECK(!mods_romfs_override("../outside.txt", host, sizeof host));  // (would leave the mod's romfs/)
    CHECK(!mods_romfs_override("/etc/passwd", host, sizeof host));
    CHECK(!mods_romfs_override("", host, sizeof host));
    char dirs[MAX_MODS][1100];
    CHECK_EQ(mods_get_active_romfs_dirs(dirs, MAX_MODS), 1);
    CHECK_STR(dirs[0], "mods/Alpha/romfs");
    // A disabled mod's files are not served, even if its path is first.
    mods_set_enabled(a, false);
    CHECK(!mods_romfs_override("a.txt", host, sizeof host));
    CHECK_EQ(mods_get_active_romfs_dirs(dirs, MAX_MODS), 0);
}

static void test_order_and_disabled(void) {
    mods_settings_load("disabled_mods", " Beta ; Gamma ");
    mods_settings_load("mod_order", "Gamma, Alpha, Beta");
    mods_rescan();
    CHECK_EQ(mods_count(), 3);
    CHECK_STR(mods_get(0)->name, "Gamma");
    CHECK_STR(mods_get(1)->name, "Alpha");
    CHECK_STR(mods_get(2)->name, "Beta");
    CHECK(!mods_get(0)->enabled);
    CHECK(mods_get(1)->enabled);
    CHECK(!mods_get(2)->enabled);
    CHECK(index_of("Gamma") == 0);

    // Moving swaps neighbours and keeps the entries themselves intact.
    mods_move_down(0);
    CHECK_STR(mods_get(0)->name, "Alpha");
    CHECK_STR(mods_get(1)->name, "Gamma");
    mods_move_up(1);
    CHECK_STR(mods_get(0)->name, "Gamma");
    mods_move_up(0);  // (no-op at the top)
    CHECK_STR(mods_get(0)->name, "Gamma");
    mods_move_down(mods_count() - 1);  // (no-op at the bottom)
    CHECK_STR(mods_get(mods_count() - 1)->name, "Beta");

    // A rescan preserves the disabled set: Beta and Gamma stay disabled.
    mods_rescan();
    CHECK(!mods_get(index_of("Beta"))->enabled);
    CHECK(!mods_get(index_of("Gamma"))->enabled);
    CHECK(mods_get(index_of("Alpha"))->enabled);

    // The saved lines are the settings file's format, and reload to the same state.
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (f) {
        mods_settings_save(f);
        rewind(f);
        char buf[2048] = {0};
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        CHECK(strstr(buf, "disabled_mods = Gamma,Beta\n") != NULL);
        CHECK(strstr(buf, "mod_order = Gamma,Alpha,Beta\n") != NULL);
    }
}

static void test_priority(void) {
    // Both enabled and both have same.txt: the first in the list wins.
    mods_settings_load("mod_order", "Beta,Alpha");
    mods_settings_load("disabled_mods", "Gamma");
    mods_rescan();
    CHECK_STR(mods_get(0)->name, "Beta");
    mods_set_enabled(index_of("Beta"), true);
    mods_set_enabled(index_of("Alpha"), true);
    char host[1200];
    CHECK(mods_romfs_override("same.txt", host, sizeof host));
    CHECK_STR(host, "mods/Beta/romfs/same.txt");
    mods_move_up(index_of("Alpha"));
    CHECK_STR(mods_get(0)->name, "Alpha");
    CHECK(mods_romfs_override("same.txt", host, sizeof host));
    CHECK_STR(host, "mods/Alpha/romfs/same.txt");
    // A short output buffer is respected.
    char tiny[4] = {0};
    CHECK(mods_romfs_override("same.txt", tiny, sizeof tiny));
    CHECK_EQ(tiny[sizeof tiny - 1], 0);
}

int main(void) {
    if (!getcwd(g_old, sizeof g_old)) return 2;
    snprintf(g_tmp, sizeof g_tmp, "/tmp/nxrt-mods-XXXXXX");
    if (!mkdtemp(g_tmp) || chdir(g_tmp) != 0) {
        perror("mkdtemp/chdir");
        return 2;
    }
    make_mods();
    test_scan();
    test_order_and_disabled();
    test_priority();
    chdir(g_old);
    rm_rf(g_tmp);
    TEST_END();
}
