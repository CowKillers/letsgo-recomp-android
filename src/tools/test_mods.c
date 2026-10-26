// Several mods at once, without the game: a small RomFS image (runtime/romfs_build.c) and mods/ folders, then each
// file read back through the virtual image the game reads (runtime/services/fs.c: header, hash tables, data).
// Build (from src/): clang -Iruntime tools/test_mods.c runtime/mods.c runtime/romfs_build.c -o build/test_mods
// Run: build/test_mods <empty scratch folder>   (exit 0 = every case as expected)
#include "services/fs.c"  // (its static index / read functions are what is tested)

#include "gamesetup.h"
#include "platform.h"
#include "romfs_build.h"
#include "settings.h"

Settings g_settings;
uint64_t g_program_id;
const char *g_romfs_dir;
const char *game_name(const char *exefs) { (void)exefs; return "Eevee"; }
// (the fs service and the mod installer are not exercised here)
void ipc_return_object(IpcResp *resp, Service *s) { (void)resp, (void)s; }
Service *service_new(const char *name, ServiceHandler h, void *state) { (void)name, (void)h, (void)state; return NULL; }
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)argv, (void)log_path, (void)append_log;
    return 0;
}
int plat_proc_wait(PlatProc p, bool block) { (void)p, (void)block; return -1; }
void rt_fatal_trace(void) { exit(3); }

static void put(const char *path, const char *text) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *s = dir + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(dir, 0755); *s = '/'; }
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint8_t *vread(int fd, uint64_t off, uint64_t size) {
    uint8_t *b = calloc(1, size + 1);
    romfs_read(fd, b, size, off);
    return b;
}

// a file's text as the game sees it (NULL: not in the image)
static char *lookup(int fd, const char *path) {
    uint8_t *h = vread(fd, 0, 0x50);
    uint64_t dsz = rd64(h + 32), hsz = rd64(h + 48), fsz = rd64(h + 64), data = rd64(h + 72);
    uint8_t *dm = vread(fd, rd64(h + 24), dsz), *fh = vread(fd, rd64(h + 40), hsz), *fm = vread(fd, rd64(h + 56), fsz);
    char dirp[1024];
    snprintf(dirp, sizeof dirp, "%s", path);
    char *slash = strrchr(dirp, '/');
    const char *name = slash ? slash + 1 : path;
    if (slash) *slash = 0; else dirp[0] = 0;
    char *text = NULL;
    int64_t dir = dir_lookup(dm, dsz, dirp);
    if (dir >= 0) {
        uint32_t nl = (uint32_t)strlen(name);
        for (uint32_t e = rd32(fh + 4 * (romfs_hash((uint32_t)dir, name, nl) % (hsz / 4)));
             e != 0xFFFFFFFFu && e + 0x20 <= fsz; e = rd32(fm + e + 0x18))
            if (rd32(fm + e) == (uint32_t)dir && rd32(fm + e + 0x1C) == nl && !memcmp(fm + e + 0x20, name, nl)) {
                text = (char *)vread(fd, data + rd64(fm + e + 8), rd64(fm + e + 0x10));
                break;
            }
    }
    free(h), free(dm), free(fh), free(fm);
    return text;
}

static int failures;
static int open_with(const char *mods) {
    fs_reset();
    snprintf(g_settings.mod, sizeof g_settings.mod, "%s", mods);
    int fd = open("game/romfs.bin", O_RDONLY);
    rom_index(fd);
    return fd;
}
static void expect(int fd, const char *mods, const char *path, const char *want) {
    char *got = lookup(fd, path);
    bool ok = want ? got && !strcmp(got, want) : !got;
    printf("%s  mod = %-22s %-14s -> %s\n", ok ? "OK  " : "FAIL", mods, path, got ? got : "(absent)");
    failures += !ok;
    free(got);
}

int main(int argc, char **argv) {
    if (argc != 2 || chdir(argv[1])) { fprintf(stderr, "usage: test_mods <empty scratch folder>\n"); return 2; }
    put("game/romfs/bin/a.txt", "base-a");
    put("game/romfs/bin/b.txt", "base-b");
    put("game/romfs/bin/sub/c.txt", "base-c");
    char why[256];
    if (!romfs_build("game/romfs", "game/romfs.bin", NULL, why, sizeof why)) { printf("romfs_build: %s\n", why); return 2; }
    put("mods/Trad/romfs/bin/a.txt", "trad-a");
    put("mods/Trad/romfs/bin/n.txt", "trad-n");
    put("mods/Rando/Rando/romfs/bin/a.txt", "rando-a");  // (one folder deeper, as archives often are)
    put("mods/Rando/Rando/romfs/bin/b.txt", "rando-b");
    put("mods/Rando/Rando/romfs/bin/n.txt", "rando-n");
    put("mods/Rando/Rando/romfs/bin/sub/m.txt", "rando-m");
    // one mod for both versions, as GameBanana's most downloaded skin ships it
    put("mods/Both/Pokemon Lets Go Pikachu/010003F003A34000/romfs/bin/sub/c.txt", "pikachu-c");
    put("mods/Both/Pokemon Lets Go Eevee/0100187003A36000/romfs/bin/sub/c.txt", "eevee-c");

    const char *m = "";
    int fd = open_with(m);
    expect(fd, m, "bin/a.txt", "base-a"), expect(fd, m, "bin/n.txt", NULL);
    close(fd);
    m = "Trad";
    fd = open_with(m);
    expect(fd, m, "bin/a.txt", "trad-a"), expect(fd, m, "bin/b.txt", "base-b"), expect(fd, m, "bin/n.txt", "trad-n");
    close(fd);
    m = "Trad, Rando";
    fd = open_with(m);
    expect(fd, m, "bin/a.txt", "trad-a"), expect(fd, m, "bin/b.txt", "rando-b");
    expect(fd, m, "bin/sub/c.txt", "base-c"), expect(fd, m, "bin/n.txt", "trad-n");
    expect(fd, m, "bin/sub/m.txt", "rando-m");
    close(fd);
    m = "Rando,Trad";
    fd = open_with(m);
    expect(fd, m, "bin/a.txt", "rando-a"), expect(fd, m, "bin/n.txt", "rando-n"), expect(fd, m, "bin/b.txt", "rando-b");
    close(fd);
    m = "Missing, Trad";
    fd = open_with(m);
    expect(fd, m, "bin/a.txt", "trad-a");
    close(fd);
    m = "Both";
    fd = open_with(m);
    expect(fd, m, "bin/sub/c.txt", "eevee-c");
    close(fd);
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
