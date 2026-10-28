// Host regression for the per-scene profile switch (t_8cc15f32), with the real hooks and settings code, no game or Vulkan.
// Uses only what the base already has (runtime/settings.c, hle/fps60.c): on publication adbf12d it compiles and fails.
//   clang -D_GNU_SOURCE -I src/runtime src/tools/test_profile_switch.c -o build/test_profile_switch -lm   [-DSWITCH_ONLY]
// 1. Battles, captures and menus default to the world's height (720), not 1080.
// 2. An older settings file's profile_battle_height = 1080 (the old default the launcher wrote) follows the world.
// 3. Back in the world after a battle, with the world updating at one present out of two (seen on the Fold7 after a
//    capture at 1080p: the switch came 291 presents late), the world profile is applied within 30 presents (< 1 s at
//    30 fps) — and a menu flash in the world still does not leave it.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../runtime/hle.h"
static int (*world_hook)(Cpu *, uint64_t), (*battle_hook)(Cpu *, uint64_t);
static uint64_t frame;
void rt_register_fnhook(uint64_t a, int (*fn)(Cpu *, uint64_t)) {
    if (a == 0x70002f8bb0ull) world_hook = fn;
    if (a == 0x700028bb60ull) battle_hook = fn;
}
uint64_t rt_addr(uint64_t a) { return a; }
void rt_call_original(Cpu *c, uint64_t a) { (void)c; (void)a; }
void guest_call(Cpu *c, uint64_t a) { (void)c; (void)a; }
uint64_t renderer_game_frame(void) { return frame; }
uint64_t plat_now_ns(void) { return 0; }
int rt_present_interval(void) { return 2; }
size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n) { size_t count = len < n - 1 ? len : n - 1; memcpy(dst, src, count); dst[count] = 0; }
    return len;
}
bool bind_settings_load(const char *key, const char *value) { (void)key; (void)value; return false; }
void bind_settings_save(FILE *f) { (void)f; }
#include "../runtime/settings.c"
#include "../runtime/hle/fps60.c"

static void present(bool world) {
    if (world) world_hook(NULL, FIELD_UPDATE);
    fps60_present();
    frame++;
}

int main(void) {
#ifndef SWITCH_ONLY  // (-DSWITCH_ONLY: only part 3, e.g. to show the late switch on a tree with the old defaults)
    assert(g_settings.profile_battle_height == g_settings.profile_world_height && g_settings.profile_world_height == 720);
    puts("DEFAULT_OK: battle / capture / menus at the world's 720p by default");

    // An older launcher file (keys as the Fold7's larecomp.ini of 28/10): the 1080 there is the old default.
    char path[] = "./profile-switch-test-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    const char *old = "fps = 30\nprofile_world_height = 720\nprofile_battle_height = 1080\nprofiles_auto = 1\n";
    assert(write(fd, old, strlen(old)) == (ssize_t)strlen(old)); close(fd);
    setenv("LARECOMP_SETTINGS", path, 1);
    settings_load();
    assert(g_settings.profiles_auto && g_settings.profile_battle_height == 720);
    // A height chosen after this change is written with the version and kept, 1080 included.
    g_settings.profile_battle_height = 1080;
    settings_save();
    g_settings.profile_battle_height = 0;
    FILE *f = fopen(path, "r"); assert(f);
    char line[256];
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\n")] = 0; parse_line(line); }
    fclose(f); unlink(path);
    assert(g_settings.profile_battle_height == 1080);
    puts("UPGRADE_OK: an older file's 1080 follows the world (720); a 1080 saved since stays 1080");
#endif

    // The live switch, with a battle profile that differs from the world's (an explicit 1080).
    g_settings.fps = 30;
    g_settings.profiles_auto = true;
    g_settings.profile_world_height = 720; g_settings.profile_battle_height = 1080;
    g_settings.profile_world_smaa = g_settings.profile_battle_smaa = false;
    g_settings.upscaler = UPSCALE_BILINEAR;  // (nothing to prewarm)
    fps60_configure();
    assert(world_hook && battle_hook);
    for (int i = 0; i < 60; i++) present(true);
    assert(settings_profile_live_height() == 720);
    battle_hook(NULL, BATTLE_START);
    for (int i = 0; i < 300; i++) present(false);   // a battle / capture: no world update
    assert(settings_profile_live_height() == 1080);
    uint64_t back = frame;                          // "[scene] world again": the world updates one present out of two
    int took = -1;
    for (int i = 0; i < 400; i++) {
        present(i % 2 == 0);
        if (took < 0 && settings_profile_live_height() == 720) took = (int)(frame - back);
    }
    printf("world profile %d presents after the world came back (world updating one present out of two)\n", took);
    assert(took >= 0 && took <= 30);
    // In the world, a short menu flash (fewer than 24 presents without a world update) keeps the world profile...
    for (int i = 0; i < 10; i++) present(false);
    for (int i = 0; i < 10; i++) present(true);
    assert(settings_profile_live_height() == 720);
    // ...a world flash inside a battle does not leave the battle profile...
    battle_hook(NULL, BATTLE_START);
    for (int i = 0; i < 40; i++) present(false);
    assert(settings_profile_live_height() == 1080);
    present(true); present(false); present(false); present(true);
    for (int i = 0; i < 30; i++) present(false);
    assert(settings_profile_live_height() == 1080);
    // ...and the world back with an update at every present switches within 4 presents.
    back = frame; took = -1;
    for (int i = 0; i < 10; i++) { present(true); if (took < 0 && settings_profile_live_height() == 720) took = (int)(frame - back); }
    assert(took >= 1 && took <= 4);
    puts("SWITCH_OK: world profile < 1 s after a battle even with intermittent world updates; flashes ignored both ways");
    return 0;
}
