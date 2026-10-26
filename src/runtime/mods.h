// Mods: game files replaced or added from a folder, as Atmosphere's LayeredFS (fan translations come this way), and
// the game's language. runtime/mods.c; the start screen (runtime/ui/launcher_lg.mm) picks both.
//
//   mods/<name>/romfs/...                                  the game's RomFS paths (bin/message/English/...)
//   mods/<name>/atmosphere/contents/<title id>/romfs/...   (or any folder holding a romfs/ up to 4 levels down:
//                                                           a download unpacked as it came)
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODS_MAX 32
// The folders under mods/ that hold a romfs/: names[i] (up to MODS_MAX). Returns how many.
int mods_list(char names[][128], int max);
// Several mods at once: g_settings.mod lists them separated by commas, the first listed wins a file several replace.
#define MODS_ACTIVE_MAX 8
// The active mods' romfs/ folders, in that order (missing ones left out). Returns how many (up to max).
int mods_active(const char *dirs[], int max);
// The first active mod's romfs/ folder, or NULL.
const char *mod_romfs_dir(void);
// path: a RomFS path ("bin/message/English/..."). True with the host file of the first active mod that has it.
bool mod_romfs_override(const char *path, char *out, size_t cap);
// The language whose texts a mod replaces (its romfs/bin/message/<language>/), as a language code ("en-US"), or NULL
// (none, or several).
const char *mod_language(const char *name);
// Installs a mod from a file the player downloaded (.zip / .rar / .7z, unpacked with Windows' tar) or a folder, on a
// background thread: its romfs/ (Let's Go Eevee's first) is copied to mods/<file name>/romfs.
enum { MOD_INSTALL_IDLE, MOD_INSTALL_RUNNING, MOD_INSTALL_DONE, MOD_INSTALL_FAIL_UNPACK, MOD_INSTALL_FAIL_NOT_MOD };
void mod_install_start(const char *src);
int mod_install_state(void);
const char *mod_install_name(void);  // after MOD_INSTALL_DONE: the mods/ folder name
// The language the game is told (nn::settings / applet "desired language"): the first active mod's with one, else
// g_settings.game_lang.
const char *game_language(void);

#ifdef __cplusplus
}
#endif
