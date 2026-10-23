// nx-ui: the app's menu (Dear ImGui), the start screen, the on-screen keyboard and the UI's own settings. It draws
// through a backend the host provides (NxUiBackend: a window and an ImGui renderer - Metal, Vulkan, D3D11...) and
// knows the rest of the app only through what is registered here: pages (tabs), rows on its own pages, HUD overlays,
// and the start screen's providers (the game files, boot profiles, a first-play question).
//
// The runtime (nx-runtime: runtime/rt_ui.mm) is one host; any other runtime can be another.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Window SDL_Window;

// ------------------------------------------------------------------ drawing backend (the host's)
// `frame` is the backend's own: what frame_begin returned (start screen), or what the host passes to menu_render
// (the in-game overlay, drawn into the host's own frame).
typedef struct NxUiBackend {
    SDL_Window *(*window)(void);
    bool (*init)(void);                    // the ImGui renderer backend, for the current ImGui context
    void (*shutdown)(void);
    void (*new_frame)(void *frame);        // the renderer backend's NewFrame
    void (*render)(void *frame);           // ImGui::GetDrawData() into the frame
    void *(*frame_begin)(const float clear[4]);             // a start screen frame (NULL: skip this one)
    void (*frame_end)(void *frame, const char *shot_path);  // present it (saved to a PNG first when shot_path)
    uint64_t (*texture_rgba)(const uint8_t *rgba, int w, int h);  // ImTextureID (0 = failed)
} NxUiBackend;
void nxui_set_backend(const NxUiBackend *b);

void nxui_init(void);  // registers the UI's settings section (nx_settings.h): before settings_load

// ------------------------------------------------------------------ the UI's own settings
typedef struct NxUiSettings {
    bool fullscreen;
    bool show_fps;       // a frame rate counter in the corner
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
    int menu_confirm;    // which button confirms in the menus: NXUI_CONFIRM_GAME (A, the game's layout) or the controller convention
} NxUiSettings;
enum { NXUI_CONFIRM_GAME = 0, NXUI_CONFIRM_CONTROLLER = 1 };
extern NxUiSettings g_ui_settings;
void nxui_set_fullscreen(bool on);  // (F11 / Alt+Enter too) applied by menu_apply_window_requests

// ------------------------------------------------------------------ the menu's pages
// System tabs. Built in: Controls (id 8, first), Saves (id 10, order 30) and Quit (id 9, last, in game only).
// Registered tabs go between, by `order`; then the game's tab (below), then Quit.
typedef struct NxUiTab {
    const char *name;
    int id;                      // stable: test hooks select tabs by id
    int order;                   // lower first
    bool needs_game;             // greyed out on the start screen (no game loaded)
    bool plain;                  // not option rows (option_rows.h): Down from the tab bar focuses its first control
    bool (*draw)(float scale);   // render thread, inside the page: true closes the menu
} NxUiTab;
void nxui_add_tab(const NxUiTab *t);  // (the tab must outlive the program: static)

// The game's tab: one tab, named after the game, after the system tabs. Everything game-specific goes in it, as its
// pages (sub-tabs, switched with ZL / ZR or [ / ]); system tabs hold system options only. A page's id is a tab id too
// (test hooks: requesting it opens the game's tab on that page).
void nxui_set_game_tab(const char *name);  // its name (default "Game")
void nxui_add_game_page(const NxUiTab *page);

// Rows on the built-in Controls page (the system's: the runtime's system language)
enum {
    NXUI_ROWS_MOVEMENT,  // more rows under "D-pad moves the player" (menu_toggle / menu_hint): system options only
    NXUI_ROWS_CONTROLS,  // whole sections after Button prompts (menu_header + rows): system options only
};
void nxui_add_rows(int where, void (*rows)(void));

// HUD overlays, drawn while the menu is closed (render thread). font: ImFont *
void nxui_add_overlay(bool (*wanted)(void), void (*draw)(float w, float h, void *font));
void nxui_on_menu_open(void (*fn)(void));
// The Quit page's "Quit to Start Screen" (NULL: only "Quit to Desktop")
void nxui_set_quit_to_start(void (*fn)(void));

// ------------------------------------------------------------------ the start screen
// The game files: where they come from, setting them up, their state. All but status may be NULL (no setup UI).
typedef struct NxUiGameFiles {
    void (*status)(char *out, size_t cap);  // "" = ready to play, else what's wrong (the status line)
    bool (*copied)(void);                   // set up can rebuild from the copy, without a folder
    bool (*busy)(void);                     // setting up (background)
    float (*progress)(void);                // 0..1 while busy
    bool (*recompiling)(void);              // the busy step is building the game code
    void (*job)(char *line, size_t n, char *hint, size_t hn, bool *failed);  // the step / result, what to do next
    void (*set_job)(const char *line, bool failed, const char *hint);
    void (*pick)(const char *folder);       // the folder the player chose
    void (*picked)(char *out, size_t cap);  // "" = none yet
    void (*start)(void);                    // set up from the chosen folder (background)
    double (*size_gb)(void);
    bool (*remove)(void);                   // delete the app's copy: true = removed
    bool (*background)(uint8_t **rgba, int *w, int *h);  // a still for the background (malloc'd), false = none
    void (*setup_rows)(float scale, bool busy);  // more option rows on the Game Files page, before Set up (e.g. where)
    void (*prelaunch)(float scale);         // Settings' first tab on the start screen ("Prelaunch")
    bool (*files_deleted)(void);            // (Prelaunch deleted them: true once)
    const char *source_desc;                // the folder row's explanation (which game, which version)
    const char *setup_desc;                 // the Set up row's explanation
    const char *ready_note;                 // under "Game files ready." ("Nintendo Switch · v1.0.0")
    // tests
    void (*test_fail)(int kind);            // show setup failure `kind` (1..6)
    void (*test_extract)(const char *folder);  // set up now, from folder (blocking)
} NxUiGameFiles;
void nxui_set_game_files(const NxUiGameFiles *g);

// Shader preparation on the start screen (runtime/nvn/shader_prep.h): after the game files are set up, and in the
// background on later starts; PLAY waits for it while the launcher draws its bar. Skipped when there is no romfs
// folder to read (a game package) or NXRT_SHADER_PREP=0.
void nxui_shader_prep_run(void);        // do it now, blocking (the setup thread, after the files are ready)
void nxui_shader_prep_start(void);      // start it in the background, once (any thread)
bool nxui_shader_prep_busy(void);
float nxui_shader_prep_progress(void);  // 0..1
void nxui_shader_prep_skip(void);       // stop it early (the shaders already built stay)

// Boot profiles next to "Vanilla" on PLAY (e.g. randomizer seeds)
typedef struct NxUiProfiles {
    int (*list)(char names[][128], int max);
    const char *(*get)(void);
    void (*set)(const char *name);  // "" = vanilla (the next boot)
    const char *label;              // before a profile's name on PLAY ("Rando")
} NxUiProfiles;
void nxui_set_profiles(const NxUiProfiles *p);

// A question before the first PLAY (e.g. the aspect ratio): its popup is "##firstplay" (BeginPopupModal); draw
// returns true once answered.
typedef struct NxUiFirstPlay {
    bool (*pending)(void);
    bool (*draw)(float scale);
    void (*test_reset)(void);  // (NXRT_LAUNCHER_SHOT=<f>:aspect) ask again
} NxUiFirstPlay;
void nxui_set_first_play(const NxUiFirstPlay *q);

typedef struct NxUiStartInfo {
    const char *headline;      // the big title ("LAS Recompiled")
    const char *releases_api;  // GitHub API URL of the release list for the update check (NULL: none)
    const char *release_page;  // release page URL, the tag appended
} NxUiStartInfo;
void nxui_set_start_info(const NxUiStartInfo *s);

// Runs the start screen until PLAY (QUIT exits). The backend's window must exist.
void launcher_run(void);
void launcher_play_profile(const char *profile);  // a page's Play on the start screen: start with that profile

// Start screen fonts, for pages it shows (ImFont *): 0 small, 1 menu, 2 title
void *nxui_start_font(int which);

#ifdef __cplusplus
}
#endif
