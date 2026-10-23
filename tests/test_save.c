// Save folders and backups (save/save.c): NXRT_SAVE_DIR / the boot profile's save folder, the once-per-boot backup,
// keeping the newest N, the list the Saves page shows, and the read/write listeners.
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "nx_save.h"
#include "nx_settings.h"

// save.c registers a settings section; this test does not read settings.
void nx_settings_register(const NxSettingsSection *s) {
    (void)s;
}

static char g_tmp[512], g_old[1024];

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

static char *read_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static char b[4096];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}

// save.c calls plat_remove_tree (nx-platform) to prune old backups.
bool plat_remove_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char c[1200];
                snprintf(c, sizeof c, "%s/%s", p, e->d_name);
                plat_remove_tree(c);
            }
            closedir(d);
        }
        return rmdir(p) == 0;
    }
    return remove(p) == 0;
}

static void rm_rf(const char *p) {
    plat_remove_tree(p);
}

static const char *profile(void) {
    return "profile";
}

static void test_dir(void) {
    unsetenv("NXRT_SAVE_DIR");
    nx_save_set_profile_dir(NULL);
    CHECK_STR(nx_save_dir(), "save");
    nx_save_set_profile_dir(profile);
    CHECK_STR(nx_save_dir(), "profile/save");
    setenv("NXRT_SAVE_DIR", "scratch", 1);
    CHECK_STR(nx_save_dir(), "scratch");
    unsetenv("NXRT_SAVE_DIR");
}

static void test_backups(void) {
    mkdir_p("profile/save");
    write_file("profile/save/slot1.dat", "one");
    write_file("profile/save/slot2.dat", "two");
    // Backups are skipped in NXRT_SAVE_DIR runs (the test-scratch mode).
    setenv("NXRT_SAVE_DIR", "scratch", 1);
    CHECK_EQ(nx_save_backup(), -1);
    unsetenv("NXRT_SAVE_DIR");

    setenv("NXRT_SAVE_BACKUPS", "2", 1);
    nx_save_reset();
    int n = nx_save_backup();
    CHECK_EQ(n, 2);                  // two files copied
    CHECK_EQ(nx_save_backup(), -1);  // once per boot
    char names[16][32];
    char dir[600];
    int count = nx_save_backups(names, 16, dir, sizeof dir);
    CHECK_EQ(count, 1);
    CHECK_STR(dir, "save_backups/profile/save");
    char p[900];
    snprintf(p, sizeof p, "%s/%s/slot1.dat", dir, count ? names[0] : "");
    if (count) CHECK_STR(read_file(p), "one");

    // Old backups: the newest `keep` stay (the new one was made just now). Start from an empty backups folder.
    rm_rf("save_backups/profile/save");
    mkdir_p("save_backups/profile/save/20200101-000000");
    mkdir_p("save_backups/profile/save/20200102-000000");
    mkdir_p("save_backups/profile/save/20200103-000000");
    // (a same-second backup returns -1 and prunes nothing; wait so this one is a new name)
    sleep(1);
    CHECK(nx_save_backup_now() >= 0);
    count = nx_save_backups(names, 16, dir, sizeof dir);
    CHECK_EQ(count, 2);
    if (count == 2) {
        CHECK_STR(names[1], "20200103-000000");  // the newest of the old ones
        CHECK(strcmp(names[0], names[1]) > 0);   // newest first
    }
    CHECK(access("save_backups/profile/save/20200101-000000", F_OK) != 0);
    CHECK(access("save_backups/profile/save/20200102-000000", F_OK) != 0);

    // The list respects max, and a missing backups folder is empty.
    char one[1][32];
    CHECK_EQ(nx_save_backups(one, 1, NULL, 0), 1);
    setenv("NXRT_SAVE_BACKUPS", "0", 1);
    CHECK_EQ(nx_save_backup_now(), -1);  // 0: backups off
    unsetenv("NXRT_SAVE_BACKUPS");
    g_save_backups_keep = 5;
    nx_save_reset();
}

static char g_last_path[600];
static uint64_t g_last_addr;
static int64_t g_last_off, g_last_size;
static int g_reads, g_writes;

static void on_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    snprintf(g_last_path, sizeof g_last_path, "%s", path);
    g_last_addr = addr, g_last_off = off, g_last_size = size;
    g_reads++;
}
static void on_write(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path, (void)addr, (void)off, (void)size;
    g_writes++;
}

static void test_listeners(void) {
    nx_save_on_read(on_read);
    nx_save_on_write(on_write);
    nx_save_read_done("profile/save/slot1.dat", 0x1234, 16, 32);
    CHECK_EQ(g_reads, 1);
    CHECK_STR(g_last_path, "profile/save/slot1.dat");
    CHECK_EQ(g_last_addr, 0x1234);
    CHECK_EQ(g_last_off, 16);
    CHECK_EQ(g_last_size, 32);
    nx_save_write_done("x", 0, 0, 0);
    CHECK_EQ(g_writes, 1);
}

int main(void) {
    if (!getcwd(g_old, sizeof g_old)) return 2;
    snprintf(g_tmp, sizeof g_tmp, "/tmp/nxrt-save-XXXXXX");
    if (!mkdtemp(g_tmp) || chdir(g_tmp) != 0) {
        perror("mkdtemp/chdir");
        return 2;
    }
    test_dir();
    test_backups();
    test_listeners();
    chdir(g_old);
    rm_rf(g_tmp);
    TEST_END();
}
