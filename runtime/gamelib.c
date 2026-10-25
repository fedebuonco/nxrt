// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"
#include "nxgame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

// The library's id file (<library>.id, written next to it by the build): the id a library for this game folder has
// (GAMELIB_ID-<build id>, gamelib_full_id), read without loading it.
static bool id_matches(const char *lib, const char *want) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return !strcmp(id, want);
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The places to look, in order (gamelib.h). Returns how many were filled.
// Android: the app's internal storage (the data folder is on external storage, mounted without exec), then one packed
// into the APK itself, opened by its bare name (it lives inside the APK: no file to look at, its own id decides).
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    const char *env = getenv("NXRT_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
    char exe_side[1024];
    if (rt_resource(nx_gamelib_name(), NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, nx_gamelib_name());
    return n;
}

static bool in_apk(const char *c) { return !strchr(c, '/'); }  // (Android: a bare name)

static const GameLibTables *open_lib(const char *path, const char *want);  // (below)

// A library that is really there: its id matches and it loads. The stub `make` leaves next to the executable in a
// source checkout has a matching id but no functions (its generated symbols are undefined), so the id file alone
// would call the game ready and then fail to boot it.
static bool lib_usable(const char *path, const char *want) {
    const GameLibTables *t = open_lib(path, want);
    if (!t) return false;
    for (uint32_t k = 0; k < t->nmodules; k++)
        if (t->modules[k].count && *t->modules[k].count) return true;
    return false;  // (loads, but holds no recompiled code)
}

// (--status: rt_ui.mm. Not in gamelib.h: that header is part of the game code's id, nxrt.mk GAMELIB_SRCS)
const char *gamelib_build_id(void);
bool gamelib_wanted_id(const char *game_dir, char *out, size_t size);
const char *gamelib_build_id(void) { return GAMELIB_ID; }
bool gamelib_wanted_id(const char *game_dir, char *out, size_t size) { return gamelib_full_id(game_dir, out, size); }

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100], want[GAMELIB_FULL_ID_SIZE];
    if (!game_dir || !gamelib_full_id(game_dir, want, sizeof want)) return false;
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++) {
        if (in_apk(c[i])) {  // (loading it is the only way to look: the library stays loaded for the game)
            char why[8];
            if (gamelib_load(NULL, why, sizeof why)) return true;
            continue;
        }
        if (exists(c[i]) && id_matches(c[i], want) && lib_usable(c[i], want)) return true;
    }
    return false;
}

static const GameLibTables *open_lib(const char *path, const char *want) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || !want || strcmp(t->id, want)) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[3][1100], want[GAMELIB_FULL_ID_SIZE] = "";
        if (!game_dir || !gamelib_full_id(game_dir, want, sizeof want))
            snprintf(g_error, sizeof g_error, "the game files' exefs/main is missing or unreadable");
        int n = game_dir ? candidates(game_dir, c) : 0;
        const char *env = getenv("NXRT_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            if (!in_apk(c[i]) && !exists(c[i])) continue;
            // NXRT_GAME_LIB / the APK's own: loaded as given, the library's own id decides
            bool given = (i == 0 && env && *env) || in_apk(c[i]);
            if (!given && !id_matches(c[i], want)) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i], want);
            if (g_tables) {
                LOG("[gamelib] %s (", c[i]);
                for (uint32_t k = 0; k < g_tables->nmodules; k++)
                    LOG("%s%s %llu", k ? " + " : "", g_tables->modules[k].name, (unsigned long long)*g_tables->modules[k].count);
                LOG(" functions)\n");
            }
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
