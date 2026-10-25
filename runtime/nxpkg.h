// The player's game package (NSP / XCI / program NCA) read in place (runtime/nxpkg.c): ExeFS extraction and the
// RomFS served straight from the package - decrypted (AES-CTR) and decompressed (BucketTree / LZ4, the layer
// hactool can't read) on the fly, so no 8 GB romfs.bin is written. The same layout as tools/nca_extract.py.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Setup: finds the program NCA in `package` (keys: the player's prod.keys; title.keys next to it or a ticket in the
// NSP for titlekey-encrypted NCAs), writes its ExeFS files into <out_dir>/exefs/ and the RomFS description
// <out_dir>/romfs.src (the package's path, where the RomFS section is, its key: what nx_romfs_open needs).
// title_id: the program's, out (may be NULL). false = failed (why: a short message for the player).
bool nx_setup(const char *package, const char *keys, const char *out_dir, uint64_t *title_id, char *why, size_t why_size);
// The same with an update (its package: .nsp): the ExeFS is the update's, the RomFS the base game's with the update's
// changes (BKTR), both read in place. update NULL: nx_setup. A package that is itself an update is refused by nx_setup.
bool nx_setup_update(const char *package, const char *update, const char *keys, const char *out_dir, uint64_t *title_id,
                     char *why, size_t why_size);

// What a game package is, without setting it up: its program's title id and main build id, and from its control data
// the game's name, publisher, version and icon (a JPEG). keys: the player's prod.keys (title.keys beside it).
typedef struct {
    uint64_t title_id;
    uint8_t build_id[8];   // exefs/main's (the first 8 bytes): which game version, as NxGameInfo.build_id
    bool has_build_id;
    char name[0x201], publisher[0x101], version[0x11];  // (empty: no control data)
    uint8_t *icon;         // JPEG, malloc'd (nx_package_info_free), NULL: none
    size_t icon_size;
    bool is_update;        // an update (its RomFS: changes to the base game's): set up with nx_setup_update
} NxPackageInfo;
bool nx_package_info(const char *package, const char *keys, NxPackageInfo *out, char *why, size_t why_size);
void nx_package_info_free(NxPackageInfo *info);

typedef struct NxRomfs NxRomfs;
// From a romfs.src written by nx_setup. NULL = missing / the package moved or changed (why).
NxRomfs *nx_romfs_open(const char *src_path, char *why, size_t why_size);
uint64_t nx_romfs_size(const NxRomfs *r);
// pread of the RomFS image; thread-safe. Returns size, or -1 (an I/O error / corrupt package).
ssize_t nx_romfs_pread(NxRomfs *r, void *dst, uint64_t size, uint64_t off);
void nx_romfs_close(NxRomfs *r);
// A file of the RomFS by its path ("nx/movie/jpn/MV_OP.webm"): where its data is in the image and its size.
bool nx_romfs_find(NxRomfs *r, const char *path, uint64_t *off, uint64_t *size);
// Every file of the RomFS, depth first: fn(path, image offset, size, ctx); returns the count, -1 on a read error.
int nx_romfs_walk(NxRomfs *r, void (*fn)(const char *path, uint64_t off, uint64_t size, void *ctx), void *ctx);
// The same over any RomFS image: rd(ctx, dst, n, off) reads n bytes of it at off (returns n, or < 0).
typedef ssize_t (*NxRomRead)(void *ctx, void *dst, uint64_t n, uint64_t off);
bool nx_romtab_find(NxRomRead rd, void *rc, const char *path, uint64_t *off, uint64_t *size);
int nx_romtab_walk(NxRomRead rd, void *rc, void (*fn)(const char *path, uint64_t off, uint64_t size, void *ctx), void *ctx);

// (tests) the AES / LZ4 primitives
void nx_aes128_ecb_decrypt(const uint8_t key[16], uint8_t *data, size_t len);
void nx_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t len);
int nx_lz4_decompress(const uint8_t *src, int src_len, uint8_t *dst, int dst_len);

#ifdef __cplusplus
}
#endif
