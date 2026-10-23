// Where the app keeps its files (nx-platform: platform/paths.c). Names come from nx_app (nx_app.h).
//
// Data root: everything the app writes or the user brings - extracted/ (game files), rando/ (seeds), save/,
// cache/ (shaders / pipelines), fonts/ (imported), the settings file. rt_paths_init() picks it and makes it the
// working directory, so the rest of the runtime opens those by their relative names. In order:
//   1. NXRT_DATA_ROOT (tests, scratch installs)
//   2. dev / portable mode: the starting directory already has extracted/ (a source checkout: `./build/<exe>
//      extracted`, the test scripts) - used as is
//   3. macOS: ~/Library/Application Support/<bundle_id> (the .app bundle itself is never written to)
//      Windows: the folder of the .exe (portable, no installer); refuses to start, with a message, if not writable
//      Linux: $XDG_DATA_HOME/<data_dir> (~/.local/share/<data_dir>)
//
// Resources: files that ship with the app (helper programs, the window icon) - next to the executable (the
// Windows folder, the .app's Contents/MacOS), in the .app's Contents/Resources, or in the source tree.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void rt_paths_init(void);         // first thing in main(), after plat_init
const char *rt_data_root(void);   // absolute
bool rt_dev_mode(void);           // dev / portable mode (rule 2 above): e.g. a source checkout
// Before rt_paths_init (the session log): the data root by the rules above; false in dev mode
bool rt_data_root_early(char *out, size_t n);

// Choosing another data folder (the start screen). NULL = it can be changed; else why not (rules 1 / 2).
const char *rt_data_root_fixed(void);
typedef struct {
    char path[1100];               // where the data would go: the folder picked, or a "LAS Recomp" folder in it
                                   // when it holds other things
    unsigned long long bytes;      // the data now (saves, seeds, settings, fonts, caches, game files)
    unsigned long long need, free; // space a move needs there (little on the same drive: hard links), space there
    bool has_data;                 // LAS Recomp data already there: used as is, not moved into
    char refuse[160];              // why it can't be the data folder ("" = it can)
} RtDataTarget;
void rt_data_target(const char *picked, RtDataTarget *t);
// The data (rt_data_target's) copied there, every file checked against the original, the switch made (from the next
// start), then the old copy removed. Nothing is removed before the switch: interrupted, the next start uses the old
// folder and removes the unfinished copy (or, after the switch, finishes removing the old one). progress: step 0
// copying, 1 checking, 2 removing the old copy. false: err says why; the data stays where it was.
bool rt_data_move(const char *to, void (*progress)(int step, double frac), char *err, size_t n);
// Only the switch: from the next start the data folder is `to` (created if needed), as it is. The old one is kept.
bool rt_data_use(const char *to, char *err, size_t n);
// Start again (the same command line, from the starting folder): after a switch. Returns only on failure.
void rt_paths_restart(void);

// A shipped file: `name` next to the executable or in Contents/Resources, else `dev_rel` in the source tree (the
// starting directory or the executable's parent; NULL = shipped builds only). Absolute path in out; false = none.
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n);
// dev_rel of the runtime's own files, in a source checkout: the Makefile passes NXRT as it was spelled (nxrt.mk).
// ../../nxrt is the monorepo layout; a project with nxrt at its root sets NXRT := nxrt, and so on.
#ifndef NXRT_DEV
#define NXRT_DEV "../../"
#endif

// The game folder: where the app keeps the game files it set up and what it builds from them (the data image, the
// game code). Default "extracted" in the data root; the player may choose another (e.g. on a portable drive), absolute.
// owns_source: the app copied exefs/ and romfs/ there; false = they are the player's own files (the game is kept
// with them): never deleted or counted, only what the app built.
void rt_game_dir_set(const char *dir, bool owns_source);
const char *rt_game_dir(void);
bool rt_game_dir_owns_source(void);

// The only way the app deletes game files. Removes the named entry of game_dir (an extracted/ folder) only if
// game_dir is <data root>/extracted or the game folder (rt_game_dir; symlinks resolved, a symlinked one is refused) and
// the entry is one the setup creates (exefs, romfs, romfs.bin, leftover folders) and not a link. The player's own
// exefs/ and romfs/ (a game folder that doesn't own them) are never deleted. true = removed or not there.
bool rt_remove_game_file(const char *game_dir, const char *name);
// Another file the host creates in extracted/ (e.g. the game code library): rt_remove_game_file may delete it too.
void rt_game_files_add(const char *name);
// All the game files set up there (rt_remove_game_file on each, then the empty folder). Saves, seeds, settings stay.
bool rt_remove_game_files(const char *game_dir);
// Bytes taken by the extracted game files in game_dir (links not followed).
unsigned long long rt_game_files_size(const char *game_dir);
// Bytes taken by everything under path (links not followed).
unsigned long long rt_tree_size(const char *path);

typedef struct SDL_Window SDL_Window;
void rt_window_icon(SDL_Window *w);  // the shipped icon.png, if any

#ifdef __cplusplus
}
#endif
