// Mods: LayeredFS-style replacement files and the game's language (runtime/mods.h). The RomFS reader
// (runtime/services/fs.c) asks mod_romfs_override for every file of the image and serves the mod's copy instead, and
// adds the files a mod has that the game hasn't.
#include "mods.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>  // rmdir (bionic does not pull it in transitively)

#include "gamesetup.h"
#include "hle.h"
#include "platform.h"
#include "romfs_build.h"
#include "settings.h"

static bool is_dir(const char *p) {
    struct stat sb;
    return !stat(p, &sb) && S_ISDIR(sb.st_mode);
}

// The romfs/ folder inside dir, up to `depth` levels down (a download unpacked with its own folders around it). Packs
// made for both games (atmosphere/titles/<Pikachu>/ and <Eevee>/) are searched under the set-up game's title id first.
#define EEVEE_TITLE "0100187003A36000"
#define PIKACHU_TITLE "010003F003A34000"
// strict: never inside a folder named after the other version's title id. A mod made for both versions
// ("Pokemon Lets Go Eevee/0100187003A36000/romfs" next to "Pokemon Lets Go Pikachu/010003F003A34000/romfs") gave the
// first folder the disk listed, so the other version's files could be served.
static bool find_romfs_in(const char *dir, int depth, bool strict, char *out, size_t cap) {
    char p[1024];
    snprintf(p, sizeof p, "%s/romfs", dir);  // (RomFs / romfs: Windows names ignore case)
    if (is_dir(p)) { snprintf(out, cap, "%s", p); return true; }
    if (depth <= 0) return false;
    const char *name = game_name("extracted/exefs");
    bool pikachu = name && !strcmp(name, "Pikachu");
    const char *mine = pikachu ? PIKACHU_TITLE : EEVEE_TITLE, *other = pikachu ? EEVEE_TITLE : PIKACHU_TITLE;
    snprintf(p, sizeof p, "%s/%s", dir, mine);
    if (is_dir(p) && find_romfs_in(p, depth - 1, strict, out, cap)) return true;
    snprintf(p, sizeof p, "%s/%s", dir, other);
    if (!strict && is_dir(p) && find_romfs_in(p, depth - 1, strict, out, cap)) return true;
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (e->d_name[0] == '.' || (strict && !strcasecmp(e->d_name, other))) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) found = find_romfs_in(p, depth - 1, strict, out, cap);
    }
    closedir(d);
    return found;
}

// The romfs/ folder of a mod: this version's if the mod has one, else any.
static bool find_romfs(const char *dir, int depth, char *out, size_t cap) {
    return find_romfs_in(dir, depth, true, out, cap) || find_romfs_in(dir, depth, false, out, cap);
}

int mods_list(char names[][128], int max) {
    DIR *d = opendir("mods");
    if (!d) return 0;
    int n = 0;
    char p[1024], r[1024];
    for (struct dirent *e; n < max && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "mods/%s", e->d_name);
        if (is_dir(p) && find_romfs(p, 4, r, sizeof r)) snprintf(names[n++], 128, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

// The romfs/ folder of mods/<name>/, or NULL. (cached: asked for every file of the image at boot)
#define ROMFS_CACHE 16
static const char *romfs_of(const char *name) {
    static struct { char name[128], dir[1024]; bool ok; } cache[ROMFS_CACHE];
    static int ncache;
    if (!name || !*name) return NULL;
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].name, name)) return cache[i].ok ? cache[i].dir : NULL;
    int slot = ncache < ROMFS_CACHE ? ncache++ : ROMFS_CACHE - 1;
    snprintf(cache[slot].name, sizeof cache[slot].name, "%s", name);
    char p[1024];
    snprintf(p, sizeof p, "mods/%s", name);
    cache[slot].ok = find_romfs(p, 4, cache[slot].dir, sizeof cache[slot].dir);
    if (cache[slot].ok) LOG("[mods] %s: %s\n", name, cache[slot].dir);
    else LOG("[mods] %s: no romfs/ folder found under mods/%s\n", name, name);
    return cache[slot].ok ? cache[slot].dir : NULL;
}

// g_settings.mod: one mod, or several separated by commas ("mod = Translation, Randomizer"). The first one listed
// wins a file several of them replace: the order is the player's, fixed (not the folder order of the disk).
static char g_active_list[sizeof g_settings.mod];
static const char *g_active[MODS_ACTIVE_MAX];
static int g_nactive;

int mods_active(const char *dirs[], int max) {
    if (strcmp(g_active_list, g_settings.mod)) {
        snprintf(g_active_list, sizeof g_active_list, "%s", g_settings.mod);
        g_nactive = 0;
        char list[sizeof g_settings.mod];
        snprintf(list, sizeof list, "%s", g_settings.mod);
        const char *names[MODS_ACTIVE_MAX];
        int nnames = 0;
        for (char *save = NULL, *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ' || *t == '\t') t++;
            for (size_t n = strlen(t); n && (t[n - 1] == ' ' || t[n - 1] == '\t'); n--) t[n - 1] = 0;
            bool dup = false;
            for (int i = 0; i < nnames; i++) dup |= !strcmp(names[i], t);
            if (!*t || dup) continue;
            if (nnames == MODS_ACTIVE_MAX) { LOG("[mods] more than %d mods active: %s and after ignored\n", MODS_ACTIVE_MAX, t); break; }
            names[nnames++] = t;
            const char *dir = romfs_of(t);
            if (dir) g_active[g_nactive++] = dir;
            char exefs[1100];  // code patches (IPS / pchtxt) are made for one build of the game: not applied here
            snprintf(exefs, sizeof exefs, "%s/../exefs", dir ? dir : "");
            if (dir && is_dir(exefs)) LOG("[mods] %s: exefs/ ignored (only romfs/ files are replaced)\n", t);
        }
    }
    int n = g_nactive < max ? g_nactive : max;
    for (int i = 0; i < n; i++) dirs[i] = g_active[i];
    return n;
}

const char *mod_romfs_dir(void) {
    const char *d[1];
    return mods_active(d, 1) ? d[0] : NULL;
}

bool mod_romfs_override(const char *path, char *out, size_t cap) {
    const char *dirs[MODS_ACTIVE_MAX];
    int n = mods_active(dirs, MODS_ACTIVE_MAX);
    struct stat sb;
    for (int i = 0; i < n; i++) {
        snprintf(out, cap, "%s/%s", dirs[i], path);
        if (!stat(out, &sb) && S_ISREG(sb.st_mode)) return true;
    }
    return false;
}

// bin/message/<folder> -> language code
static const struct { const char *folder, *code; } k_msg_langs[] = {
    {"English", "en-US"}, {"Spanish", "es"},  {"French", "fr"},          {"German", "de"},         {"Italian", "it"},
    {"JPN", "ja"},        {"Korean", "ko"},   {"Simp_Chinese", "zh-CN"}, {"Trad_Chinese", "zh-TW"},
};

const char *mod_language(const char *name) {
    const char *root = romfs_of(name);
    if (!root) return NULL;
    const char *found = NULL;
    char p[1100];
    for (size_t i = 0; i < sizeof k_msg_langs / sizeof k_msg_langs[0]; i++) {
        snprintf(p, sizeof p, "%s/bin/message/%s", root, k_msg_langs[i].folder);
        if (!is_dir(p)) continue;
        if (found) return NULL;  // several languages: the player's choice stands
        found = k_msg_langs[i].code;
    }
    return found;
}

const char *game_language(void) {
    char list[sizeof g_settings.mod];  // the first active mod with a language of its own
    snprintf(list, sizeof list, "%s", g_settings.mod);
    for (char *save = NULL, *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        while (*t == ' ' || *t == '\t') t++;
        for (size_t n = strlen(t); n && (t[n - 1] == ' ' || t[n - 1] == '\t'); n--) t[n - 1] = 0;
        const char *m = mod_language(t);
        if (m) return m;
    }
    // Fan translations REPLACE the English files (keeping the folder name "English").
    // The game must request en-US to load the translated PT-BR text.
    return g_settings.game_lang[0] ? g_settings.game_lang : "en-US";
}

// ------------------------------------------------------------------ installing a mod (the start screen's button)
static void rm_tree(const char *p) {  // (only ever called on folders under mods/)
    DIR *d = opendir(p);
    if (!d) { remove(p); return; }
    char c[1100];
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if (is_dir(c)) rm_tree(c);
        else remove(c);
    }
    closedir(d);
    rmdir(p);
}

static _Atomic int g_install_state;
static char g_install_src[1024], g_install_name[128];
int mod_install_state(void) { return g_install_state; }
const char *mod_install_name(void) { return g_install_name; }

static void *install_thread(void *arg) {
    (void)arg;
    const char *src = g_install_src, *base = src;
    for (const char *c = src; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    // the name: the file / folder name without its extension, letters, digits, spaces, '-' and '.' only
    char name[128];
    int n = 0;
    for (const char *c = base; *c && n < 60; c++) {
        unsigned char ch = (unsigned char)*c;
        name[n++] = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == ' ' || ch == '.' ? (char)ch : '_';
    }
    name[n] = 0;
    char *dot = strrchr(name, '.');
    if (dot && !is_dir(src)) *dot = 0;
    while (n && name[strlen(name) - 1] == ' ') name[strlen(name) - 1] = 0;
    if (!name[0]) snprintf(name, sizeof name, "mod");
    mkdir("mods", 0755);
    const char *root = src;
    if (!is_dir(src)) {  // an archive: Windows' own tar (libarchive: .zip, .rar, .7z) into mods/.install
        rm_tree("mods/.install");
        mkdir("mods/.install", 0755);
        char tar[600];
        const char *sys = getenv("SystemRoot");
        snprintf(tar, sizeof tar, "%s\\System32\\tar.exe", sys ? sys : "C:\\Windows");
        const char *argv[] = {tar, "-xf", src, "-C", "mods/.install", NULL};
        PlatProc p = plat_spawn(argv, "mods/.install.log", false);
        int r = p ? plat_proc_wait(p, true) : -1;
        remove("mods/.install.log");
        if (r != 0) { LOG("[mods] couldn't unpack %s (tar %d)\n", src, r); g_install_state = MOD_INSTALL_FAIL_UNPACK; goto done; }
        root = "mods/.install";
    }
    {
        char romfs[1024];
        if (!find_romfs(root, 6, romfs, sizeof romfs)) { g_install_state = MOD_INSTALL_FAIL_NOT_MOD; goto done; }
        char dst[300];
        snprintf(dst, sizeof dst, "mods/%s", name);
        if (is_dir(dst)) rm_tree(dst);
        mkdir(dst, 0755);
        strlcat(dst, "/romfs", sizeof dst);
        if (!romfs_copy_tree(romfs, dst, NULL)) { g_install_state = MOD_INSTALL_FAIL_UNPACK; goto done; }
        snprintf(g_install_name, sizeof g_install_name, "%s", name);
        LOG("[mods] installed %s from %s\n", name, src);
        g_install_state = MOD_INSTALL_DONE;
    }
done:
    rm_tree("mods/.install");
    return NULL;
}

void mod_install_start(const char *src) {
    if (g_install_state == MOD_INSTALL_RUNNING) return;
    snprintf(g_install_src, sizeof g_install_src, "%s", src);
    g_install_name[0] = 0;
    g_install_state = MOD_INSTALL_RUNNING;
    pthread_t t;
    pthread_create(&t, NULL, install_thread, NULL);
    pthread_detach(t);
}
