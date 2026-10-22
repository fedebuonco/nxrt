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

// A shipped file: `name` next to the executable or in Contents/Resources, else `dev_rel` in the source tree (the
// starting directory or the executable's parent; NULL = shipped builds only). Absolute path in out; false = none.
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n);
// dev_rel of the runtime's own files, in a source checkout: the Makefile passes NXRT as it was spelled (nxrt.mk).
// ../../nxrt is the monorepo layout; a project with nxrt at its root sets NXRT := nxrt, and so on.
#ifndef NXRT_DEV
#define NXRT_DEV "../../nxrt/"
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

typedef struct SDL_Window SDL_Window;
void rt_window_icon(SDL_Window *w);  // the shipped icon.png, if any

#ifdef __cplusplus
}
#endif
