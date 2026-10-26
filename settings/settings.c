#include "nx_settings.h"

#include <stdlib.h>
#include <string.h>

#include "nx_app.h"
#include "nx_log.h"

enum { MAX_SECTIONS = 16 };
static const NxSettingsSection *g_sections[MAX_SECTIONS];
static int g_nsections;

void nx_settings_register(const NxSettingsSection *s) {
    for (int i = 0; i < g_nsections; i++)
        if (g_sections[i] == s) return;
    if (g_nsections < MAX_SECTIONS) g_sections[g_nsections++] = s;
    else NX_LOGE(NX_LOG_SETTINGS, "too many sections: %s left out", s->name);
}

// NXRT_SETTINGS=<path>: another settings file (test runs never touch the player's)
const char *nx_settings_path(void) {
    const char *e = getenv("NXRT_SETTINGS");
    return e && *e ? e : nx_app.ini;
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
    if (f) NX_LOGI(NX_LOG_SETTINGS, "loaded %s", nx_settings_path());
    else NX_LOGD(NX_LOG_SETTINGS, "%s not found; defaults", nx_settings_path());
    if (getenv("NXRT_SETTINGS_ROUNDTRIP")) {  // (test) write it back the way this build would, and stop
        settings_save();
        exit(0);
    }
}

void settings_save(void) {
    FILE *f = fopen(nx_settings_path(), "w");
    if (!f) { NX_LOGE(NX_LOG_SETTINGS, "cannot write %s", nx_settings_path()); return; }
    fprintf(f, "# %s settings (edited from the in-game menu: F1, Esc, or Minus+Plus on a controller)\n", nx_app.title);
    for (int i = 0; i < g_nsections; i++)
        if (g_sections[i]->write) g_sections[i]->write(f);
    fclose(f);
    NX_LOGI(NX_LOG_SETTINGS, "wrote %s", nx_settings_path());
}
