// User settings (larecomp.ini in the data folder), set on the start screen (runtime/ui/launcher_lg.mm).
#pragma once
#include <stdbool.h>

enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
// (11 was 67% of the display: larecomp.ini "67%" now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_FSR1 = 2 /* AMD FSR 1 */ };
#define UPSCALE_SHARP UPSCALE_FSR1  // the non-bilinear upscaler (the default)
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { PASP_GAME = 0, PASP_43 = 1, PASP_FILL = 2 };  // image format of a settings profile
enum { VREAD_AUTO = 0, VREAD_COPY = 1, VREAD_DIRECT = 2 };

typedef struct {
    bool fullscreen;
    int fps;  // 30 (the game's own) or 60 (interpolated: runtime/nvn/vulkan/exec.c)
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int res_height;      // the launcher's render height in lines (720 / 1080 / 1440 / 2160); 0: res_mode decides
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    float mouse_sens;    // mouse aim (the virtual gyro, runtime/hle/input.c): degrees per pixel
    float stick_deadzone;  // a controller's sticks: the share of their travel read as rest (runtime/hle/input.c)
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool show_fps;
    bool dpad_move;      // the d-pad (and arrow keys) move the player like the stick
    bool render_thread;  // the backend runs on its own thread (runtime/nvn/vulkan/renderthread.c); default on
    int vertex_read;     // VREAD_*: where the GPU reads vertex and index data (runtime/nvn/vulkan/exec.c)
    bool wild_battle;    // touching a wild Pokémon starts a battle, then the capture (runtime/hle/wildbattle.c)
    int profile_world_height;   // WORLD profile: internal render height; 0 = shared render_height
    int profile_battle_height;  // BATTLE/MENUS profile: internal render height; 0 = shared render_height
    bool profile_world_smaa;    // WORLD profile anti-aliasing
    bool profile_battle_smaa;   // BATTLE/MENUS profile anti-aliasing
    bool profiles_auto;         // switch the profiles live, by scene (internal height + SMAA); off = cold requests only
    int profile_world_aspect;   // PASP_* of the WORLD profile
    int profile_battle_aspect;  // PASP_* of the BATTLE/MENUS profile
    char game_lang[8];   // the game's language code ("en-US", "es"...: runtime/mods.c game_language)
    char mod[512];       // active mods / fan translations (mods/<name>/, "a, b": several), empty = none (runtime/mods.h)
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
typedef enum { RT_SCENE_MENU = 0, RT_SCENE_WORLD = 1, RT_SCENE_BATTLE = 2 } RtScene;
RtScene fps60_profile_scene(void);
void settings_profile_apply(void);
bool settings_profiles_rejected(void);  // true while the live switch is off (profiles_auto = 0)
// Live profile (profiles_auto): written by the game thread at each present, read by the render thread.
int settings_profile_live_height(void);  // internal render height in lines; 0 = no override (shared render_height)
int settings_profile_live_smaa(void);    // 1 / 0 = SMAA forced on / off; -1 = no override (g_settings.antialias)
bool settings_profile_prewarm_wanted(void);        // the render thread should build the post pipelines before the first switch
void settings_profile_set_prewarmed(bool done);    // ... and says so here: until then a switch is refused
