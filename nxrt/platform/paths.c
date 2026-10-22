// Data root and shipped resources (runtime/paths.h).
#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "paths.h"
#include "nx_app.h"
#include "platform.h"

static char g_root[1024], g_start[1024], g_exe_dir[1024];

#ifdef _WIN32
#define SEP "\\"
static bool is_link(const char *p) {  // symlink or junction
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
static bool resolve(const char *p, char *out, size_t n) { return _fullpath(out, p, n) != NULL; }
#define path_eq(a, b) (_stricmp(a, b) == 0)
#else
#define SEP "/"
static bool is_link(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}
static bool resolve(const char *p, char *out, size_t n) {
    char buf[PATH_MAX];
    if (!realpath(p, buf)) return false;
    snprintf(out, n, "%s", buf);
    return true;
}
#define path_eq(a, b) (strcmp(a, b) == 0)
#endif

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') { char s = *c; *c = 0; mkdir(p, 0755); *c = s; }
    mkdir(p, 0755);
}

static bool writable(const char *dir) {
    char t[1100];
    snprintf(t, sizeof t, "%s" SEP ".write_test", dir);
    FILE *f = fopen(t, "wb");
    if (!f) return false;
    fclose(f);
    remove(t);
    return true;
}

static void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, nx_app.title, msg, NULL);
    exit(1);
}

static bool g_dev;  // the data root is the starting folder because it has extracted/ (a source checkout)
bool rt_dev_mode(void) { return g_dev; }

void rt_paths_init(void) {
    if (!getcwd(g_start, sizeof g_start)) g_start[0] = 0;
    if (plat_exe_path(g_exe_dir, sizeof g_exe_dir)) {
        char *s = strrchr(g_exe_dir, '/'), *b = strrchr(g_exe_dir, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
    }
    char root[1024];
    const char *env = getenv("NXRT_DATA_ROOT");
    if (env && *env) snprintf(root, sizeof root, "%s", env);
    else if (is_dir("extracted")) snprintf(root, sizeof root, "%s", g_start), g_dev = true;  // dev / portable: as before
    else {
#ifdef _WIN32
        snprintf(root, sizeof root, "%s", g_exe_dir);
#elif defined(__linux__)
        // (the AppImage is read-only): $XDG_DATA_HOME/<data_dir>, ~/.local/share/<data_dir> by default
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (xdg && *xdg == '/') snprintf(root, sizeof root, "%s/%s", xdg, nx_app.data_dir);
        else snprintf(root, sizeof root, "%s/.local/share/%s", home && *home ? home : "/tmp", nx_app.data_dir);
#else
        const char *home = getenv("HOME");
        snprintf(root, sizeof root, "%s/Library/Application Support/%s", home && *home ? home : "/tmp", nx_app.bundle_id);
#endif
    }
    mkdirs(root);
    char msg[1400];
    if (!is_dir(root) || !writable(root) || chdir(root) != 0) {
#ifdef _WIN32
        snprintf(msg, sizeof msg, "%s can't write to its folder:\n%s\n\nIt keeps game files, saves and settings "
                 "next to the program. Move the \"%s\" folder somewhere you can write to (e.g. Documents or "
                 "Desktop, not Program Files) and start it again.", nx_app.title, root, nx_app.title);
#else
        snprintf(msg, sizeof msg, "%s can't write to its data folder:\n%s", nx_app.title, root);
#endif
        fail(msg);
    }
    if (!getcwd(g_root, sizeof g_root)) snprintf(g_root, sizeof g_root, "%s", root);
    fprintf(stderr, "[paths] data: %s\n", g_root);
}

const char *rt_data_root(void) { return g_root; }

bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) {
    char c[4][1100] = {{0}};
    if (name) {
        snprintf(c[0], sizeof c[0], "%s" SEP "%s", g_exe_dir, name);
        snprintf(c[1], sizeof c[1], "%s" SEP ".." SEP "Resources" SEP "%s", g_exe_dir, name);  // .app/Contents/Resources
    }
    if (dev_rel) {
        snprintf(c[2], sizeof c[2], "%s" SEP "%s", g_start, dev_rel);
        snprintf(c[3], sizeof c[3], "%s" SEP ".." SEP "%s", g_exe_dir, dev_rel);  // build/<exe> -> the game's source folder
    }
    for (int i = 0; i < 4; i++)
        if (c[i][0] && exists(c[i])) return (size_t)snprintf(out, n, "%s", c[i]) < n;  // (as is: a venv python must stay a link)
    return false;
}

// ---- game files
// What the setup (runtime/ui/launcher.mm) creates in extracted/, plus what older versions' extraction and manual
// extractions left there (never read at run time).
static const char *const GAME_FILES[] = {"exefs", "romfs", "romfs.bin", "romfs.src", ".gamebuild", ".extract", "extract.log",
                                         "xci_secure", "nso_raw", "update_pfs", ".DS_Store"};

// the host's own (rt_game_files_add: e.g. the game code library it builds there)
static char g_extra[8][80];
static int g_nextra;
void rt_game_files_add(const char *name) {
    if (g_nextra < 8) snprintf(g_extra[g_nextra++], sizeof g_extra[0], "%s", name);
}

// ---- the game folder: <data root>/extracted, or where the player chose to keep the game (e.g. a portable drive)
static char g_game_dir[1100] = "extracted";
static bool g_owns_source = true;
void rt_game_dir_set(const char *dir, bool owns_source) {
    snprintf(g_game_dir, sizeof g_game_dir, "%s", dir && *dir ? dir : "extracted");
    g_owns_source = owns_source;
}
const char *rt_game_dir(void) { return g_game_dir; }
bool rt_game_dir_owns_source(void) { return g_owns_source; }

// game_dir must be <data root>/extracted or the chosen game folder, a real folder (a dev worktree's symlinked
// extracted/ is refused)
static bool game_dir_ok(const char *game_dir) {
    char want[1100], got[1100], chosen[1100];
    snprintf(want, sizeof want, "%s" SEP "extracted", g_root);
    if (!g_root[0] || is_link(game_dir) || !resolve(game_dir, got, sizeof got)) return false;
    if (path_eq(want, got)) return true;
    return !is_link(g_game_dir) && resolve(g_game_dir, chosen, sizeof chosen) && path_eq(chosen, got);
}

// The player's own files in a game folder the app didn't fill (the game kept with the player's files): never deleted
static bool players_own(const char *game_dir, const char *name) {
    char got[1100], chosen[1100];
    if (g_owns_source || !resolve(game_dir, got, sizeof got) || !resolve(g_game_dir, chosen, sizeof chosen) ||
        !path_eq(got, chosen))
        return false;
    return !strcmp(name, "exefs") || !strcmp(name, "romfs") || !strcmp(name, ".DS_Store");
}

bool rt_remove_game_file(const char *game_dir, const char *name) {
    bool known = false;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) known |= !strcmp(name, GAME_FILES[i]);
    for (int i = 0; i < g_nextra; i++) known |= !strcmp(name, g_extra[i]);
    if (!known || !game_dir_ok(game_dir) || players_own(game_dir, name)) {
        fprintf(stderr, "[paths] refusing to delete %s/%s (not app-created game files in the game folder)\n", game_dir, name);
        return false;
    }
    char p[1100];
    snprintf(p, sizeof p, "%s" SEP "%s", game_dir, name);
    if (!exists(p) && !is_link(p)) return true;
    if (is_link(p)) {
        fprintf(stderr, "[paths] refusing to delete %s (a link)\n", p);
        return false;
    }
    return is_dir(p) ? plat_remove_tree(p) : remove(p) == 0;  // (plat_remove_tree doesn't follow links inside)
}

bool rt_remove_game_files(const char *game_dir) {
    if (!game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete game files in %s (not the game folder)\n", game_dir);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++)
        if (!players_own(game_dir, GAME_FILES[i])) ok &= rt_remove_game_file(game_dir, GAME_FILES[i]);
    for (int i = 0; i < g_nextra; i++) ok &= rt_remove_game_file(game_dir, g_extra[i]);
    rmdir(game_dir);  // only if empty: anything else in there stays
    return ok;
}

static unsigned long long tree_size(const char *p) {
    struct stat st;
    if (is_link(p) || stat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
    unsigned long long n = 0;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[2048];
            snprintf(c, sizeof c, "%s" SEP "%s", p, e->d_name);
            n += tree_size(c);
        }
        closedir(d);
    }
    return n;
}

unsigned long long rt_game_files_size(const char *game_dir) {
    unsigned long long n = 0;
    char p[1100];
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) {
        if (players_own(game_dir, GAME_FILES[i])) continue;  // (only what the app made counts)
        snprintf(p, sizeof p, "%s" SEP "%s", game_dir, GAME_FILES[i]);
        n += tree_size(p);
    }
    for (int i = 0; i < g_nextra; i++) {
        snprintf(p, sizeof p, "%s" SEP "%s", game_dir, g_extra[i]);
        n += tree_size(p);
    }
    return n;
}

// Window / Dock icon: icon.png shipped next to the exe or in the .app (none for a plain source build).
void rt_window_icon(SDL_Window *w) {
    char p[1100];
    if (!rt_resource("icon.png", NULL, p, sizeof p)) return;
    SDL_Surface *s = SDL_LoadPNG(p);
    if (s) SDL_SetWindowIcon(w, s), SDL_DestroySurface(s);
}
