#include "nx_settings.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "nx_app.h"

enum { MAX_SECTIONS = 16 };
static const NxSettingsSection *g_sections[MAX_SECTIONS];
static int g_nsections;

void nx_settings_register(const NxSettingsSection *s) {
    for (int i = 0; i < g_nsections; i++)
        if (g_sections[i] == s) return;
    if (g_nsections < MAX_SECTIONS) g_sections[g_nsections++] = s;
    else fprintf(stderr, "[settings] too many sections: %s left out\n", s->name);
}

// NXRT_SETTINGS=<path>: another settings file (test runs never touch the player's)
const char *nx_settings_path(void) {
    const char *e = getenv("NXRT_SETTINGS");
    return e && *e ? e : nx_app.ini;
}

int g_default_upscaler = UPSCALE_SHARP;
int g_default_res_mode = RES_MATCH;
bool g_gpu_integrated;

static const char *res_name(int m) {
    static char n[8];
    if (m >= 1 && m <= 8) { snprintf(n, sizeof n, "%dx", m); return n; }
    return m == RES_DYNAMIC ? "dynamic" : m == RES_Q75 ? "75%" : m == RES_Q50 ? "50%" : m == RES_Q25 ? "25%" : "match";
}

void settings_resolve_defaults(bool integrated) {
    const char *e = getenv("NXRT_INTEGRATED");
    if (e && *e) integrated = atoi(e) != 0;
    g_gpu_integrated = integrated;
    g_default_upscaler = integrated ? UPSCALE_BILINEAR : UPSCALE_SHARP;
    g_default_res_mode = RES_MATCH;  // (Dynamic is not a default until it is reliable: user 2026-10-18)
    if (g_settings.upscaler_auto) g_settings.upscaler = g_default_upscaler;
    if (g_settings.res_auto) g_settings.res_mode = g_default_res_mode;
    LOG("[settings] %s GPU: upscaler %s (%s), resolution %s (%s), anti-aliasing %s\n", integrated ? "integrated" : "discrete / other",
        g_settings.upscaler == UPSCALE_BILINEAR ? "bilinear" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "metalfx",
        g_settings.upscaler_auto ? "default" : "chosen", res_name(g_settings.res_mode), g_settings.res_auto ? "default" : "chosen",
        g_settings.antialias == AA_SMAA ? "smaa" : "off");
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    for (int i = 0; i < g_nsections; i++)
        if (g_sections[i]->defaults) g_sections[i]->defaults();
    FILE *f = fopen(nx_settings_path(), "r");
    if (f) {
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;  // (Windows: binary stdio keeps the CR of CRLF)
            char key[64];
            char val[1024];
            int got = sscanf(line, " %63[^= ] = %1023[^\n]", key, val);
            if (got < 1 || key[0] == '#') continue;
            if (got < 2) val[0] = 0;  // ("key =": an empty value)
            for (int i = 0; i < g_nsections; i++)
                if (g_sections[i]->key && g_sections[i]->key(key, val)) break;
        }
        fclose(f);
    }
    for (int i = 0; i < g_nsections; i++)
        if (g_sections[i]->loaded) g_sections[i]->loaded(f != NULL);
    if (f) fprintf(stderr, "[settings] loaded %s\n", nx_settings_path());
    if (getenv("NXRT_SETTINGS_ROUNDTRIP")) {  // (test) write it back the way this build would, and stop
        settings_save();
        exit(0);
    }
}

void settings_save(void) {
    FILE *f = fopen(nx_settings_path(), "w");
    if (!f) { fprintf(stderr, "[settings] cannot write %s\n", nx_settings_path()); return; }
    fprintf(f, "# %s settings (edited from the in-game menu: F1, Esc, or Minus+Plus on a controller)\n", nx_app.title);
    for (int i = 0; i < g_nsections; i++)
        if (g_sections[i]->write) g_sections[i]->write(f);
    fclose(f);
}
