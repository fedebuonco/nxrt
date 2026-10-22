// nx-save: where the player's saves live (in the data root: nx-platform paths.h), per boot profile, with backups,
// and who gets told when the game reads or writes them. The host's file system (the runtime: services/fs.c) asks
// nx_save_dir for the folder and reports each save file read / write.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The active boot profile's folder (e.g. a randomizer seed's): its saves are <folder>/save. NULL / "": the default.
void nx_save_set_profile_dir(const char *(*dir)(void));

// The save folder for this boot: NXRT_SAVE_DIR (tests: a scratch copy), else the profile's <folder>/save, else "save".
const char *nx_save_dir(void);

void nx_save_init(void);  // registers its settings section (nx_settings.h: save_backups): before settings_load

// Copies nx_save_dir() to save_backups/<folder>/<YYYYMMDD-HHMMSS>/ and keeps the newest g_save_backups_keep (setting
// save_backups, default 5; NXRT_SAVE_BACKUPS overrides; 0 = no backups). Once per boot (later calls return at once);
// skipped for NXRT_SAVE_DIR test runs. Returns the number of files copied, -1 = none made.
int nx_save_backup(void);
int nx_save_backup_now(void);  // one more now, whatever was done this boot (the Saves page's Back up now)
extern int g_save_backups_keep;
// The backups of the current save folder, newest first (names: YYYYMMDD-HHMMSS); their folder in dir.
int nx_save_backups(char names[][32], int max, char *dir, int dir_cap);
void nx_save_reset(void);  // a new boot: the next nx_save_backup makes one again

// Save file traffic (path: the host file; addr: the guest buffer). Listeners run on the game's thread.
typedef void (*NxSaveListener)(const char *path, uint64_t addr, int64_t off, int64_t size);
void nx_save_on_read(NxSaveListener fn);   // after a read
void nx_save_on_write(NxSaveListener fn);  // after a write
void nx_save_read_done(const char *path, uint64_t addr, int64_t off, int64_t size);
void nx_save_write_done(const char *path, uint64_t addr, int64_t off, int64_t size);

#ifdef __cplusplus
}
#endif
