// The game interface: everything the shared runtime needs to know about the one game it is linked with.
//
// The runtime (`runtime/`) holds no game. A game (games/<id>/) defines exactly one `const NxGame nx_game` (who it is:
// names, title id, build id; and optional hooks), and its own HLE functions with HLE_FUNC / RT_ON_RESET (hle.h) as the
// runtime's do. Every hook may be NULL: the runtime then does what a plain Switch would (no overlay, no extra menu
// tabs, files served as they are). Hooks run on the thread named next to them.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Cpu Cpu;
typedef union SDL_Event SDL_Event;
typedef struct NvnTexture NvnTexture;

// ------------------------------------------------------------------ identity
// The game's code modules (the NSOs in exefs/ it runs) and where each one loads: main first. A module whose file isn't
// there is left out. Empty: main at 0x7000000000, sdk at 0x7004000000.
enum { NX_MAX_MODULES = 8 };
typedef struct NxModule {
    const char *name;  // its file in exefs/ ("main", "sdk", "subsdk0" ...)
    uint64_t base;     // guest address: below HLE_BASE (runtime/rt.h), modules don't overlap
} NxModule;

typedef struct NxCodeRange { uint64_t fn, end; } NxCodeRange;  // a function's guest code: [fn, end]

typedef struct NxGameInfo {
    const char *id;            // short name, file names: the game code library (<id>game.dylib / .so / .dll)
    const char *exe;           // the executable's name (logs, crash reports, the Windows import library)
    const char *title;         // the app's name, shown to the player ("LAS Recomp")
    const char *game_name;     // the game's full name, in setup texts
    const char *game_version;  // the one version of the game files it is built for ("1.0.0")
    uint64_t title_id;         // the game's program id
    uint8_t build_id[8];       // the first bytes of the main NSO's build id: the setup refuses other versions
    const char *bundle_id;     // macOS data folder (~/Library/Application Support/<this>)
    const char *data_dir;      // Linux data folder name ($XDG_DATA_HOME/<this>)
    const char *ini;           // settings file name, in the data root
    const char *version;       // the app's release version
    const char *releases_api;  // GitHub API URL of the release list for the update check (NULL: no check)
    const char *release_page;  // release page URL, the tag appended
    const char *headline;      // the start screen's big title ("LAS Recompiled")
    const char *romfs_probe;   // a RomFS path every complete copy of the game files has (setup check)
    uint32_t romfs_files;      // the RomFS's file count in a complete copy (0: the crash dialog can't tell if files are missing)
    const char *title_movie;   // RomFS path of a movie whose first frame is the start screen's background (NULL: none)
    const char *nickname;      // the console account's nickname the game reads (NULL: "Player")
    const char *menu_name;     // the game's tab in the menu ("Link's Awakening"; NULL: game_name)
    NxModule modules[NX_MAX_MODULES];  // its code modules ({0}: main + sdk at the defaults)
} NxGameInfo;

// ------------------------------------------------------------------ input: NxPad (nx-input)
#include "nx_input.h"

// ------------------------------------------------------------------ graphics (runtime/nvn: both renderers)
// One draw, as the game hooks see it. Guest memory pointers: the game may read them (and, in draw_textures, write
// the constants it is about to draw with).
enum { NX_DRAW_MAX_TEX = 8 };
typedef struct NxDraw {
    uint64_t fs_hash;               // the fragment shader (a hash of its code: stable across runs)
    const float *ubo[6];            // vertex stage uniform buffers 0..5 (NULL: not bound)
    uint32_t ubo_size[6];
    uint64_t mesh;                  // vertex buffer 0's guest address (identifies a model)
    uint32_t instances;
    bool tintable;                  // the shader takes a per-draw tint (draw_tint runs)
    int tex_ids[NX_DRAW_MAX_TEX];   // restyled textures (tex_identify) sampled by the fragment stage
    int ntex;
} NxDraw;

// ------------------------------------------------------------------ menu (runtime/ui/menu.mm)
// A page of the game's tab in the menu: the game's tab (named nx_game.info.menu_name) holds all the game's own options;
// the system tabs (Controls, Video, Audio, Saves, Quit) hold system options only and look the same in every game.
typedef struct NxMenuTab {
    const char *name;
    int id;                      // stable: test hooks select pages by id (the system tabs: 0 Video, 1 Audio, 8 Controls, 9 Quit, 10 Saves)
    bool needs_game;             // greyed out on the start screen (no game loaded)
    bool plain;                  // not option rows (runtime/ui/option_rows.h): Down from the tab bar focuses its first control
    bool (*draw)(float scale);   // render thread, inside the page: true closes the menu
} NxMenuTab;

// One HUD overlay a game draws while the menu is closed (a timer, a pickup card, a tracker). Drawn in array order,
// under the menu, above the game image (runtime/ui/menu.mm, MAX_OVERLAYS = 8).
typedef struct NxOverlay {
    bool (*wanted)(void);
    void (*draw)(float w, float h, void *font);  // ImFont *
} NxOverlay;

// ------------------------------------------------------------------ hooks
typedef struct NxGameHooks {
    // boot (main thread)
    void (*modules_loaded)(void);            // after every boot's module load, before the kernel starts
    uint32_t (*code_variant)(uint64_t addr); // generated code at a variant site (gen.py --variants): the word to run, 0 = original
    // guest code the game enters in the middle of a function (a branch into [fn, end]): those run that function
    const NxCodeRange *inner_entries;
    int ninner_entries;

    // per frame
    void (*game_tick)(Cpu *c, uint64_t frame);  // game main thread, once per frame (first GetNpadState of player 1)
    void (*present_tick)(uint64_t frame);       // presenter, after each presented frame
    bool (*in_play)(void);                      // a save is loaded and the game is running (not the title screen)
    bool (*movie_skippable)(const char *romfs_path);  // A / B (or Plus) skip this movie (the opening ...)
    const char *(*scene_name)(void);            // current level / scene, for logs and the FPS line (NULL: none)

    // input (main thread): SDL events the game sees (after the menu's), mouse capture, stick dead zone
    void (*input_event)(const SDL_Event *e);
    bool (*mouse_wanted)(void);  // capture the mouse (relative mode) while the game plays and no menu is up
    int stick_deadzone;          // controller sticks inside this radius read 0 (drift; 0 = none)
    bool keyboard_prompts;       // the game restyles its button prompts for the keyboard too (Controls > Button icons)
    // input (game main thread, GetNpadState)
    void (*pad_script)(uint64_t frame, NxPad *p);  // test scripts: may add buttons / steer the stick
    bool (*pad_mirrored)(void);                    // the world is shown mirrored: left / right swap
    void (*pad_filter)(NxPad *p);                  // after the d-pad walk, before stick directions become buttons
    void (*pad_final)(NxPad *p);                   // the state as it is handed to the game

    // files (runtime/services/fs.c)
    const char *(*content_dir)(void);  // the active overlay folder (it may hold romfs/ additions, save/), NULL = none
    bool (*romfs_override)(const char *path, char *host, size_t cap);  // a host file served instead of this RomFS file
    // The game is about to see `n` bytes of a RomFS file, read at `off` in that file: the hook may rewrite `buf`
    // (guest memory) in place and returns true when it did (a seed's room files, content patches). Called on every
    // read of RomFS bytes, so keep it a cheap check first.
    bool (*romfs_read_filter)(const char *path, uint64_t off, void *buf, size_t n);
    void (*romfs_file_opened)(const char *path);
    void (*save_read)(const char *path, uint64_t addr, int64_t off, int64_t size);     // after a save file read
    void (*save_written)(const char *path, uint64_t addr, int64_t off, int64_t size);  // before a save file write

    const char *(*language_code)(const char *code);  // the code the game takes for a system language ("zh-CN" -> "zh-Hans")
    int max_frame_rate;  // 120: the game can run at 120 FPS (Video > Frame rate; its patches follow rt_frame_rate). 0 = 60

    // settings (runtime/settings.c): the game's own keys in the same ini file
    void (*settings_defaults)(void);
    bool (*settings_key)(const char *key, const char *val);  // true = the game's key (taken)
    void (*settings_write)(FILE *f);

    // graphics: textures (renderer, render thread)
    int (*tex_identify)(const NvnTexture *t);  // a texture the game restyles: its id (>= 0), -1 = none
    uint32_t (*tex_generation)(void);          // changes when restyled textures must be uploaded again
    // restyle mip level lv of texture id in place: its linear data in its own format (rows of blocks, row bytes apart)
    void (*tex_apply)(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row);
    NvnTexture *(*tex_replace)(int id);  // a texture to sample in place of this one (a game icon over a map), or NULL
    bool (*scene_fresh_targets)(const char *scene);  // a scene that never draws its surroundings: targets start black
    // graphics: per draw (render thread), in this order. false from draw_shader / draw_textures leaves the draw out.
    bool (*draw_shader)(NxDraw *d);       // the program is known (fs_hash, ubo): before textures are bound
    bool (*draw_textures)(NxDraw *d);     // textures bound (tex_ids filled): last chance to change guest constants
    float (*draw_effect_stretch)(uint64_t fs_hash);  // a full-screen effect's horizontal stretch (1 = none)
    bool (*draw_mirror)(bool composite);  // mirror this draw (mirror mode): composite = the scene into the window
    // A draw whose shader takes a per-draw tint, as the shader sees it (ltint[4]): [0] rgb multiply + strength,
    // [1] rgb add + luma exponent, [2]/[3] a second recolour window. strength ([0].w) < 0 = the HSV recolour
    // (runtime/nvn/maxwell/translate.c emit_recolour); 0 = untouched.
    void (*draw_tint)(const NxDraw *d, float ltint[16]);
    // instanced draws (ssbo0: a 4x4 model matrix per instance): instances tinted differently are drawn in runs
    bool (*instances_tinted)(void);                     // any instance may be tinted this frame
    bool (*instance_tint)(const float *model, float tint[16]);  // true = this instance is tinted (ltint[0..1] filled, rest 0)
    bool box_depth_draws;  // widescreen: depth-tested draws into the window image are boxed into 16:9 like the UI
    // Renderer features only some games need: off unless the game asks, so no other game pays for them. (The per-draw
    // tint is on only for a game with draw_tint or instance_tint: without either, no translated shader gets its
    // binding and branch. scene_fresh_targets above: the level clears.)
    bool scene_targets_fresh;  // screen-sized targets start transparent black at their first use each frame (a game
                               // whose scenes leave parts undrawn, which would show older frames: LAS's pits)
    bool frame_rate_hold;      // runtime/nvn/pace.c: hold the game's own 30 fps mode when frames don't fit 60 (LAS's
                               // frame controller), with the timing counters it reads
    bool late_module_init;  // the modules but sdk start after the SDK's heap is up, main last (default: all but main first)
    // graphics: presenting (render thread)
    bool (*present_wide)(void);            // the scene may be shown wider than 16:9 (unlocked aspect)
    void (*present_crop)(uint64_t frame, double shape, float crop[4], double *x, double *y, double *w, double *h);
    void (*present_shape)(float shape);    // the aspect the next frames render at
    void (*present_frame_end)(uint64_t frame);

    // menu (render thread): the game's tab and its pages, in this order (all game-specific options go there)
    const NxMenuTab *menu_pages;
    int menu_npages;
    void (*menu_opened)(void);
    // HUD overlays, in this order, while the menu is closed. A port with a single overlay can keep using the pair
    // below (it is drawn after these).
    const NxOverlay *overlays;
    int noverlays;
    bool (*overlay_wanted)(void);
    void (*overlay_draw)(float w, float h, void *font);  // ImFont *

    // start screen (runtime/ui/launcher.mm): the game's boot profiles (e.g. randomizer seeds) next to "Vanilla"
    int (*profiles_list)(char names[][128], int max);  // the profiles there are now, in order
    const char *(*profile_get)(void);                  // the one the next boot uses ("" = vanilla)
    void (*profile_set)(const char *name);             // "" = vanilla (the next boot; not saved)
    const char *profile_label;                         // shown before a profile's name on PLAY ("Rando")
} NxGameHooks;

typedef struct NxGame {
    NxGameInfo info;
    NxGameHooks hooks;
} NxGame;

extern const NxGame nx_game;  // defined by the game
#define NX_HOOK(name) (nx_game.hooks.name)

// The start screen is up: no game is loaded (runtime/ui/launcher.mm); game code must not read guest memory.
extern int g_start_screen;

// Test hook: save the next presented frame to this path (cleared when saved).
extern char g_window_shot_request[256];

// The game code library's file name: <id>game.<dylib|so|dll>
const char *nx_gamelib_name(void);

#ifdef __cplusplus
}
#endif
