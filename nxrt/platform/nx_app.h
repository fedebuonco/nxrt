// Who the app is: names the lower layers need (data folder, settings file, logs, dialogs). The host (nx-runtime, or any
// other runtime) fills it in once, first thing in main(), before plat_init.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NxAppInfo {
    const char *id;         // short name ("las")
    const char *exe;        // the executable's name (logs, crash reports)
    const char *title;      // shown to the player ("LAS Recomp")
    const char *bundle_id;  // macOS data folder (~/Library/Application Support/<this>)
    const char *data_dir;   // Linux data folder name ($XDG_DATA_HOME/<this>)
    const char *ini;        // settings file name, in the data root
    const char *version;    // the app's release version
} NxAppInfo;

extern NxAppInfo nx_app;
void nx_app_init(const NxAppInfo *info);

// Test hook: the host saves its next presented frame to this path (and clears it).
extern char g_window_shot_request[256];

#ifdef __cplusplus
}
#endif
