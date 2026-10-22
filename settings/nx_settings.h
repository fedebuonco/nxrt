// The settings file (nx-settings): one ini in the data root (nx_app.ini), "key = value" lines. Each part of the app
// registers a section of its own keys (nx_settings_register), before the first settings_load; the file is read once
// and every line goes to the first section that takes its key, and settings_save writes the sections in the order they
// were registered.
#pragma once
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NxSettingsSection {
    const char *name;                             // (logs)
    void (*defaults)(void);                       // before the file is read
    bool (*key)(const char *key, const char *val);  // true = this section's key (taken). val "" for "key ="
    void (*loaded)(bool file_found);              // after the file was read (fix-ups: missing keys, ranges)
    void (*write)(FILE *f);                       // its lines
} NxSettingsSection;

void nx_settings_register(const NxSettingsSection *s);  // (the section must outlive the program: static)
void settings_load(void);   // once (later calls do nothing)
void settings_save(void);
const char *nx_settings_path(void);  // NXRT_SETTINGS=<path> (tests), else nx_app.ini

#ifdef __cplusplus
}
#endif
