#include "nx_log.h"
#include "nx_save.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "nx_settings.h"
#include "platform.h"


static const char *(*g_profile_dir)(void);
void nx_save_set_profile_dir(const char *(*dir)(void)) { g_profile_dir = dir; }

const char *nx_save_dir(void) {
    if (getenv("NXRT_SAVE_DIR")) return getenv("NXRT_SAVE_DIR");  // test runs use a scratch copy
    const char *p = g_profile_dir ? g_profile_dir() : NULL;
    if (p && *p) {  // the profile has its own save slots
        static char dir[600];
        snprintf(dir, sizeof dir, "%s/save", p);
        return dir;
    }
    return "save";
}

// ---- backups
static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdirs(const char *path) {
    char p[1100];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}

static int copy_tree(const char *src, const char *dst) {
    DIR *d = opendir(src);
    if (!d) return -1;
    mkdirs(dst);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[1100], t[1100];
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
        if (is_dir(s)) { int k = copy_tree(s, t); if (k > 0) n += k; continue; }
        FILE *in = fopen(s, "rb"), *out = in ? fopen(t, "wb") : NULL;
        if (in && out) {
            char buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, got, out);
            n++;
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }
    closedir(d);
    return n;
}

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static bool g_backed_up;
void nx_save_reset(void) { g_backed_up = false; }
int g_save_backups_keep = 5;

static void backups_root(char *root, size_t cap) {
    snprintf(root, cap, "save_backups/%s", nx_save_dir());
    for (char *c = root + strlen("save_backups/"); *c; c++)
        if (*c == '\\' || *c == ':') *c = '_';
}

static int backup(void) {
    const char *keep_s = getenv("NXRT_SAVE_BACKUPS");
    int keep = keep_s ? atoi(keep_s) : g_save_backups_keep;
    const char *src = nx_save_dir();
    if (keep <= 0 || getenv("NXRT_SAVE_DIR") || !is_dir(src)) return -1;
    char root[700], dst[800];
    backups_root(root, sizeof root);
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(dst, sizeof dst, "%s/%s", root, stamp);
    if (is_dir(dst)) return -1;  // (two boots in one second)
    int n = copy_tree(src, dst);
    NX_LOGI(NX_LOG_SAVE, "backup of %s: %d files -> %s", src, n, dst);
    // keep the newest `keep` (the names sort by time)
    static char names[256][32];
    int count = 0;
    DIR *d = opendir(root);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && count < 256)
            if (e->d_name[0] != '.' && strlen(e->d_name) < sizeof names[0]) snprintf(names[count++], sizeof names[0], "%s", e->d_name);
        closedir(d);
    }
    qsort(names, count, sizeof names[0], name_cmp);
    for (int i = 0; i + keep < count; i++) {
        char old[900];
        snprintf(old, sizeof old, "%s/%s", root, names[i]);
        plat_remove_tree(old);
    }
    return n;
}

int nx_save_backup(void) {
    if (g_backed_up) return -1;
    g_backed_up = true;
    return backup();
}
int nx_save_backup_now(void) { return backup(); }

int nx_save_backups(char out[][32], int max, char *dir, int dir_cap) {
    char root[700];
    backups_root(root, sizeof root);
    if (dir) snprintf(dir, dir_cap, "%s", root);
    static char names[256][32];
    int count = 0;
    DIR *d = opendir(root);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && count < 256)
            if (e->d_name[0] != '.' && strlen(e->d_name) < sizeof names[0]) snprintf(names[count++], sizeof names[0], "%s", e->d_name);
        closedir(d);
    }
    qsort(names, count, sizeof names[0], name_cmp);
    int n = 0;
    for (int i = count - 1; i >= 0 && n < max; i--) snprintf(out[n++], 32, "%s", names[i]);
    return n;
}

// ---- settings (nx_settings.h)
static bool save_key(const char *key, const char *val) {
    if (strcmp(key, "save_backups")) return false;
    g_save_backups_keep = atoi(val) < 0 ? 0 : atoi(val);
    return true;
}
static void save_write(FILE *f) { fprintf(f, "save_backups = %d\n", g_save_backups_keep); }
void nx_save_init(void) {
    static const NxSettingsSection s = {"save", NULL, save_key, NULL, save_write};
    nx_settings_register(&s);
}

// ---- listeners
enum { MAX_LISTENERS = 8 };
static NxSaveListener g_read[MAX_LISTENERS], g_write[MAX_LISTENERS];
static int g_nread, g_nwrite;
void nx_save_on_read(NxSaveListener fn) { if (g_nread < MAX_LISTENERS) g_read[g_nread++] = fn; }
void nx_save_on_write(NxSaveListener fn) { if (g_nwrite < MAX_LISTENERS) g_write[g_nwrite++] = fn; }
void nx_save_read_done(const char *path, uint64_t addr, int64_t off, int64_t size) {
    for (int i = 0; i < g_nread; i++) g_read[i](path, addr, off, size);
}
void nx_save_write_done(const char *path, uint64_t addr, int64_t off, int64_t size) {
    for (int i = 0; i < g_nwrite; i++) g_write[i](path, addr, off, size);
}
