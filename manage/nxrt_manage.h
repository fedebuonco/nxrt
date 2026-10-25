// libnxrt_manage: what a manager app (nxrt-frontend) needs from nxrt without the runtime - no window, no GPU, no game
// code. A game folder's files (exefs/, and romfs/ or romfs.src: the RomFS read from the player's game package in place),
// their version, setup from a package, and the RomFS's files. Everything a running game knows about itself (its id,
// whether its game code is current) comes from the game's own app instead: `<exe> --describe` and `<exe> --status`
// print one JSON object each (runtime/rt_ui.mm).
//
// A plain C ABI: every function is thread-safe on its own arguments, strings are UTF-8 paths, `why` buffers receive a
// short message for the player on failure. Built from manage/nxrt_manage.c, runtime/nxpkg.c and runtime/lz4_block.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXM_ABI 2  // (2: NxmPackage.is_update, nxm_package_setup_update)
int nxm_abi(void);  // NXM_ABI of the library linked: a manager refuses another

// ------------------------------------------------------------------ a game folder
typedef struct {
    bool exefs;          // exefs/main is there and is an NSO
    bool romfs_dir;      // romfs/ is a folder (an extracted game)
    bool romfs_package;  // romfs.src: the RomFS read from the game package in place
    uint8_t build_id[8]; // exefs/main's build id (its first 8 bytes; what NxGameInfo.build_id names)
} NxmGameFiles;
// What the folder holds. false: the folder isn't there.
bool nxm_game_files(const char *game_dir, NxmGameFiles *out);
// An NSO's build id (first 8 bytes). false: unreadable or not an NSO.
bool nxm_nso_build_id(const char *path, uint8_t out[8]);

// Set a game folder up from a game package (.nsp / .xci / program .nca): its ExeFS written to <out_dir>/exefs/, the
// RomFS described in <out_dir>/romfs.src (read from the package in place, nothing decrypted to disk). keys: the
// player's prod.keys. title_id: the package's program (may be NULL). Takes seconds (no RomFS copy).
bool nxm_package_setup(const char *package, const char *keys, const char *out_dir, uint64_t *title_id, char *why,
                       size_t why_size);
// The same from a base game and its update (an .nsp): the update's ExeFS, the base game's RomFS with the update's
// changes (both read in place). update NULL: nxm_package_setup.
bool nxm_package_setup_update(const char *package, const char *update, const char *keys, const char *out_dir,
                              uint64_t *title_id, char *why, size_t why_size);

// ------------------------------------------------------------------ a game package (.nsp / .xci / program .nca)
typedef struct {
    uint64_t title_id;
    uint8_t build_id[8];  // exefs/main's: which version of the game (NxGameInfo.build_id)
    bool has_build_id;
    char name[0x201], publisher[0x101], version[0x11];  // from its control data ("" when it has none)
    uint8_t *icon;  // JPEG (nxm_package_free), NULL: none
    size_t icon_size;
    bool is_update;  // an update: set up with its base game (nxm_package_setup_update)
} NxmPackage;
// What the package is, without setting it up (reads only its headers and control data). false: why.
bool nxm_package_info(const char *package, const char *keys, NxmPackage *out, char *why, size_t why_size);
void nxm_package_free(NxmPackage *p);
// The keys file for a package: prod.keys beside it, else ~/.switch/prod.keys. false: none found.
bool nxm_find_keys(const char *package, char *out, size_t out_size);

// ------------------------------------------------------------------ the RomFS of a game folder
typedef struct NxmRomfs NxmRomfs;
// romfs/ or romfs.src, whichever the folder has. NULL: neither (why).
NxmRomfs *nxm_romfs_open(const char *game_dir, char *why, size_t why_size);
void nxm_romfs_close(NxmRomfs *r);
// Every file: fn(path relative to the RomFS root, size, ctx). Returns the count, -1 on a read error.
int nxm_romfs_list(NxmRomfs *r, void (*fn)(const char *path, uint64_t size, void *ctx), void *ctx);
// A file's size, -1 if there is none at path.
int64_t nxm_romfs_file_size(NxmRomfs *r, const char *path);
// n bytes of the file at path from off: the bytes read (0 at its end), -1 on an error / no such file.
int64_t nxm_romfs_read(NxmRomfs *r, const char *path, uint64_t off, void *dst, uint64_t n);
// The file at path copied to out_path. false: not there / I/O error (why).
bool nxm_romfs_extract(NxmRomfs *r, const char *path, const char *out_path, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
