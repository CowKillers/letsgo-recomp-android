#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#include "paths.h"

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's larecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    return e && *e ? e : "larecomp.ini";
}

Settings g_settings = {
    .fullscreen = false,
    .fps = 30,
    .volume = 1.0f,
    .mouse_sens = 0.1f,
    .stick_deadzone = 0.15f,
    .reverb = true,
    .movie_audio = true,
    .show_fps = false,
    .render_thread = true,
    .vertex_read = VREAD_AUTO,
    .upscaler = UPSCALE_SHARP,
    .profile_world_height = 720,
    // Battles, captures and menus at the world's height by default: 1080p cost the most there (Fold7 28/10: 19 fps in
    // a capture at 1080p, 25-30 in the world at 720p with as many draws). 1080p stays one choice away in the launcher.
    .profile_battle_height = 720,
    .profile_world_smaa = false,
    .profile_battle_smaa = false,
    .profile_world_aspect = PASP_GAME,
    .profile_battle_aspect = PASP_GAME,
#ifdef __ANDROID__
    .antialias = AA_OFF,  // Android: SMAA costs 6 fullscreen passes, too heavy for mobile GPUs
#else
    .antialias = AA_SMAA,  // (the game has no anti-aliasing of its own)
#endif
};

// Unknown keys (an older larecomp.ini: Link's Awakening's options from LAS Recomp) are ignored, and dropped at the
// next save.
// Per-scene profiles (profiles_auto = 1). A switch changes two things only, both without building a shader at the
// moment of the switch: the internal height (a scaled image is re-created at its next use, the shaders do not depend on
// the size: the viewport is dynamic) and the SMAA passes (their pipelines are built by the render thread BEFORE the
// first switch: settings_profile_set_prewarmed). The image format stays cold (the camera is patched once at boot).
// Until the renderer has said it is prewarmed the switch is refused, and the refusal is logged. A new scene must hold
// for PROFILE_HOLD presents before it switches: a one-frame menu flash never reallocates the render targets.
// Back to the world the hold is shorter and tolerates gaps: right after a battle (or with the battle profile's heavier
// image) the world does not update at every present, and a hold that restarted at each present without a world update
// kept the battle profile for 291 presents (~14 s, Fold7 28/10) after "[scene] world again". The world profile now
// comes after WORLD_HOLD presents that updated the world, as long as no PROFILE_HOLD presents in a row went without one.
// Never mutate the shared settings here (the render worker may still be using the previous frame): the live values are
// atomics, read by the render thread.
#define PROFILE_HOLD 24
#define WORLD_HOLD 4
uint64_t renderer_game_frame(void);
static atomic_int g_live_height, g_live_smaa = -1, g_prewarmed;
bool settings_profiles_rejected(void) { return !g_settings.profiles_auto; }
int settings_profile_live_height(void) { return atomic_load(&g_live_height); }
int settings_profile_live_smaa(void) { return atomic_load(&g_live_smaa); }
void settings_profile_set_prewarmed(bool done) { atomic_store(&g_prewarmed, done); }
bool settings_profile_prewarm_wanted(void) {
    return g_settings.profiles_auto && (g_settings.profile_world_smaa || g_settings.profile_battle_smaa || g_settings.upscaler == UPSCALE_FSR1);
}
void settings_profile_apply(void) {
    static int last = -1, world = 0, away = 0, applied = -1;
    RtScene scene = fps60_profile_scene();
    {   // LARECOMP_PROFILE_FORCE=<presents> (Android: setprop debug.larecomp.profile_force): test aid, flips the scene every N presents (a measured switch needs no battle)
        static long force = -1;
        if (force < 0) {
            force = getenv("LARECOMP_PROFILE_FORCE") ? atol(getenv("LARECOMP_PROFILE_FORCE")) : 0;
#ifdef __ANDROID__
            char pv[PROP_VALUE_MAX] = "";  // adb shell setprop debug.larecomp.profile_force <presents>
            if (!force && __system_property_get("debug.larecomp.profile_force", pv) > 0) force = atol(pv);
#endif
        }
        if (force > 0) scene = (renderer_game_frame() / (uint64_t)force) & 1 ? RT_SCENE_BATTLE : RT_SCENE_WORLD;
    }
    if (!g_settings.profiles_auto) {
        if ((int)scene == last) return;
        last = scene;
        int want = scene == RT_SCENE_WORLD ? 0 : 1;
        LOG("[profiles] scene=%s requested=%s switch=refused reason=profiles_auto-off\n",
            scene == RT_SCENE_WORLD ? "world" : scene == RT_SCENE_BATTLE ? "battle/capture" : "menu/loading", want ? "battle-menus" : "world");
        return;
    }
    // world: presents that updated the world since the last PROFILE_HOLD presents in a row without one; away: that run
    if (scene == RT_SCENE_WORLD) { away = 0; if (world < WORLD_HOLD) world++; }
    else if (away < PROFILE_HOLD && ++away == PROFILE_HOLD) world = 0;
    int want = world >= WORLD_HOLD ? 0 : away >= PROFILE_HOLD ? 1 : applied;  // 0 = WORLD profile, 1 = BATTLE / MENUS
    if (want < 0 || want == applied) return;
    if (settings_profile_prewarm_wanted() && !atomic_load(&g_prewarmed)) {
        static bool said;
        if (!said) { said = true; LOG("[profiles] scene=%s requested=%s switch=refused reason=renderer-not-prewarmed\n",
                                      scene == RT_SCENE_WORLD ? "world" : "battle/menus", want ? "battle-menus" : "world"); }
        return;
    }
    int h = want ? g_settings.profile_battle_height : g_settings.profile_world_height;
    bool aa = want ? g_settings.profile_battle_smaa : g_settings.profile_world_smaa;
    int fmt = want ? g_settings.profile_battle_aspect : g_settings.profile_world_aspect;
    atomic_store(&g_live_height, h);
    atomic_store(&g_live_smaa, aa ? 1 : 0);
    extern long fps60_world_back(void);  // (hle/fps60.c: the present the world came back at after a battle, -1 = none)
    long back = fps60_world_back();
    uint64_t now = renderer_game_frame();
    // the first world switch after a battle / capture: how many presents after "[scene] world again" (the old hold: 291)
    static long told = -1;
    char since[48] = "";
    if (!want && back >= 0 && back != told && (uint64_t)back <= now) {
        told = back;
        snprintf(since, sizeof since, " after_world_again=%llu", (unsigned long long)(now - (uint64_t)back));
    }
    LOG("[profiles] scene=%s profile=%s switch=applied present=%llu height=%d smaa=%d format=%d(cold) prewarmed=%d%s\n",
        scene == RT_SCENE_WORLD ? "world" : scene == RT_SCENE_BATTLE ? "battle/capture" : "menu/loading", want ? "battle-menus" : "world",
        (unsigned long long)now, h, aa, fmt, atomic_load(&g_prewarmed), since);
    applied = want;
}

// profiles_version = 2: the file was written once battles default to the world's height. An older file holding
// profile_battle_height = 1080 holds the old default (the launcher and the start screen wrote every key), not a choice:
// it follows the world's height. A height picked from now on writes the version with it and stays as chosen.
#define PROFILES_VERSION 2
static int g_profiles_version;
static void profiles_upgrade(void) {
    if (g_profiles_version >= PROFILES_VERSION || g_settings.profile_battle_height != 1080) return;
    g_settings.profile_battle_height = g_settings.profile_world_height ? g_settings.profile_world_height : 720;
    LOG("[profiles] battle height 1080 from an older settings file (the old default) -> %d, like the world\n",
        g_settings.profile_battle_height);
}

static void parse_line(const char *line) {
    char key[64];
    char val[128];
    if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) < 1) return;
    if (!strcmp(key, "game_lang") || !strcmp(key, "mod")) {  // (may be empty)
        if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) != 2) val[0] = 0;
        if (key[0] == 'g') strlcpy(g_settings.game_lang, val, sizeof g_settings.game_lang);
        else strlcpy(g_settings.mod, val, sizeof g_settings.mod);
        return;
    }
    if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
    else if (!strcmp(key, "fps")) g_settings.fps = atoi(val) == 60 ? 60 : 30;
    else if (!strcmp(key, "render_height")) g_settings.res_height = atoi(val) >= 360 && atoi(val) <= 4320 ? atoi(val) : 0;
    else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
    else if (!strcmp(key, "mouse_sens")) g_settings.mouse_sens = (float)atof(val);
    else if (!strcmp(key, "stick_deadzone")) g_settings.stick_deadzone = (float)atof(val);
    else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
    else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
    else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
    else if (!strcmp(key, "resolution")) {
        int n = atoi(val);
        g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                              !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                              (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;
    }
    else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;
    // (an older "antialias = off" was only LAS Recomp's default being saved: a new key, so SMAA starts on)
    else if (!strcmp(key, "smaa")) g_settings.antialias = atoi(val) != 0 ? AA_SMAA : AA_OFF;
    else if (!strcmp(key, "render_thread")) g_settings.render_thread = atoi(val) != 0;
    else if (!strcmp(key, "vertex_read")) g_settings.vertex_read = !strcmp(val, "direct") ? VREAD_DIRECT : !strcmp(val, "copy") ? VREAD_COPY : VREAD_AUTO;
    else if (!strcmp(key, "dpad_move")) g_settings.dpad_move = atoi(val) != 0;
    else if (!strcmp(key, "wild_battle")) g_settings.wild_battle = atoi(val) != 0;
    else if (!strcmp(key, "profiles_auto")) g_settings.profiles_auto = atoi(val) != 0;
    else if (!strcmp(key, "profiles_version")) g_profiles_version = atoi(val);
    else if (!strcmp(key, "profile_world_height")) g_settings.profile_world_height = atoi(val) >= 360 && atoi(val) <= 4320 ? atoi(val) : 0;
    else if (!strcmp(key, "profile_battle_height")) g_settings.profile_battle_height = atoi(val) >= 360 && atoi(val) <= 4320 ? atoi(val) : 0;
    else if (!strcmp(key, "profile_world_smaa")) g_settings.profile_world_smaa = atoi(val) != 0;
    else if (!strcmp(key, "profile_battle_smaa")) g_settings.profile_battle_smaa = atoi(val) != 0;
    else if (!strcmp(key, "profile_world_aspect")) g_settings.profile_world_aspect = atoi(val) == PASP_43 ? PASP_43 : atoi(val) == PASP_FILL ? PASP_FILL : PASP_GAME;
    else if (!strcmp(key, "profile_battle_aspect")) g_settings.profile_battle_aspect = atoi(val) == PASP_43 ? PASP_43 : atoi(val) == PASP_FILL ? PASP_FILL : PASP_GAME;
    else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
#ifdef __ANDROID__
    g_settings.fullscreen = true;  // Android port: always start fullscreen (immersive)
#endif
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        // (a file edited by hand may have CRLF: "resolution = dynamic\r" did not match and fell back silently)
        for (size_t n = strlen(line); n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '); n--) line[n - 1] = 0;
        // (0.3.1 saved some settings without their line break - "fullscreen = 0fps = 60render_height = 1440": a key
        // glued to the previous value is split off again, so a file written then keeps its settings)
        const char *p = line;
        for (;;) {
            const char *eq = strstr(p, " = "), *next = eq ? strstr(eq + 3, " = ") : NULL, *k = next;
            while (k && k > eq + 3 && ((k[-1] >= 'a' && k[-1] <= 'z') || k[-1] == '_')) k--;
            if (!next || k == next || k <= eq + 3) { parse_line(p); break; }
            char part[256];
            snprintf(part, sizeof part, "%.*s", (int)(k - p), p);
            parse_line(part);
            p = k;
        }
    }
    fclose(f);
    profiles_upgrade();
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
#ifdef __ANDROID__
    g_settings.fullscreen = true;  // (an old larecomp.ini with fullscreen = 0 must not win)
#endif
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# " RT_APP_NAME " settings (set on the start screen)\n");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "fps = %d\n", g_settings.fps);
    fprintf(f, "render_height = %d\n", g_settings.res_height);
    fprintf(f, "smaa = %d\n", g_settings.antialias == AA_SMAA);
    fprintf(f, "mod = %s\n", g_settings.mod);
    fprintf(f, "mouse_sens = %.3f\n", g_settings.mouse_sens);
    fprintf(f, "stick_deadzone = %.2f\n", g_settings.stick_deadzone);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "render_thread = %d\n", g_settings.render_thread);
    fprintf(f, "vertex_read = %s\n", g_settings.vertex_read == VREAD_DIRECT ? "direct" : g_settings.vertex_read == VREAD_COPY ? "copy" : "auto");
    fprintf(f, "dpad_move = %d\n", g_settings.dpad_move);
    fprintf(f, "wild_battle = %d\n", g_settings.wild_battle);
    fprintf(f, "profiles_auto = %d\n", g_settings.profiles_auto);
    fprintf(f, "profiles_version = %d\n", PROFILES_VERSION);
    fprintf(f, "profile_world_height = %d\n", g_settings.profile_world_height);
    fprintf(f, "profile_battle_height = %d\n", g_settings.profile_battle_height);
    fprintf(f, "profile_world_smaa = %d\n", g_settings.profile_world_smaa);
    fprintf(f, "profile_battle_smaa = %d\n", g_settings.profile_battle_smaa);
    fprintf(f, "profile_world_aspect = %d\n", g_settings.profile_world_aspect);
    fprintf(f, "profile_battle_aspect = %d\n", g_settings.profile_battle_aspect);
    fprintf(f, "game_lang = %s\n", g_settings.game_lang);
    bind_settings_save(f);
    fclose(f);
}
