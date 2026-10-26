// Let's Go Recomp start screen: the game files (picked once, set up and recompiled here: runtime/gamesetup.c), the
// resolution, fullscreen, anti-aliasing, the translation (mods/, installed from a downloaded file: runtime/mods.c),
// the keyboard / mouse controls, Play. In English, or in Portuguese when the system's language is (LARECOMP_LANG=en|pt
// chooses). The choices go to larecomp.ini (settings.c; bind_key_* from runtime/hle/bindings.c).
// Tests: LARECOMP_LAUNCHER_SHOT=<frame> saves build/launcher.png (or LARECOMP_SHOT_PATH) and exits;
// LARECOMP_LAUNCHER_AUTOPLAY=1 plays as soon as the game files are ready; LARECOMP_SETUP_DIR=<folder> sets them up
// from that folder at once; LARECOMP_MOD_INSTALL=<file or folder> installs a translation at once;
// LARECOMP_LAUNCHER_KEYS=1 opens the keyboard section, =<slot> also waits for a new key for that input
// (runtime/input_bind.h BIND_*), and <slot>:<key name> presses that key for it.
#include <SDL3/SDL.h>
#include <float.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl3.h"

extern "C" {
#include "gamesetup.h"
#include "input_bind.h"
#include "mods.h"
#include "paths.h"
#include "settings.h"
void settings_save(void);
void ui_window_init(void);
SDL_Window *ui_window(void);
void menu_apply_window_requests(void *sdl_window);
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
}

// ------------------------------------------------------------------ language
static bool g_pt;
static void pick_language(void) {
    const char *e = getenv("LARECOMP_LANG");
    if (e && *e) { g_pt = !strncmp(e, "pt", 2); return; }
    int n = 0;
    SDL_Locale **l = SDL_GetPreferredLocales(&n);
    g_pt = l && n > 0 && l[0]->language && !strcmp(l[0]->language, "pt");
    SDL_free(l);
}
#define T(en, pt) (g_pt ? (pt) : (en))

// ------------------------------------------------------------------ keyboard
// The game's actions with the key bound to each. Click one, then press the new key (Esc or 5 s cancels; a key already
// in use swaps over). Controllers keep their buttons: nothing to set for them.
static const struct { int slot; const char *en, *pt; } k_actions[] = {
    {BIND_LS_UP, "Walk up", "Andar para cima"},             {BIND_A, "A (or Enter): confirm", "A (ou Enter): confirmar"},
    {BIND_LS_DOWN, "Walk down", "Andar para baixo"},         {BIND_B, "B (or Backspace): back", "B (ou Backspace): cancelar"},
    {BIND_LS_LEFT, "Walk left", "Andar para a esquerda"},    {BIND_X, "X: menu", "X: menu"},
    {BIND_LS_RIGHT, "Walk right", "Andar para a direita"},   {BIND_Y, "Y: options / box", "Y: opções / caixa"},
    {BIND_UP, "D-pad up", "Direcional cima"},                {BIND_L, "L: move Pokémon / items", "L: mover Pokémon / itens"},
    {BIND_DOWN, "D-pad down", "Direcional baixo"},           {BIND_R, "R: move Pokémon / items", "R: mover Pokémon / itens"},
    {BIND_LEFT, "D-pad left", "Direcional esquerda"},        {BIND_PLUS, "+: partner", "+: parceiro"},
    {BIND_RIGHT, "D-pad right", "Direcional direita"},       {BIND_MINUS, "-: Pokédex", "-: Pokédex"},
};

static const char *key_name(int sc) {
    switch (sc) {
    case SDL_SCANCODE_SPACE: return T("Space", "Espaço");
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_UP: return T("Up arrow", "Seta cima");
    case SDL_SCANCODE_DOWN: return T("Down arrow", "Seta baixo");
    case SDL_SCANCODE_LEFT: return T("Left arrow", "Seta esquerda");
    case SDL_SCANCODE_RIGHT: return T("Right arrow", "Seta direita");
    case 0: return T("(none)", "(nenhuma)");
    default: return bind_key_label(sc);
    }
}

static void keys_section(void) {
    int cap_dev, cap_slot, ms_left;
    bind_capture_active(&cap_dev, &cap_slot, &ms_left);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 1));
    if (ImGui::BeginTable("##keys", 4, ImGuiTableFlags_SizingFixedFit)) {
        for (size_t i = 0; i < sizeof k_actions / sizeof k_actions[0]; i++) {
            int slot = k_actions[i].slot;
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(k_actions[i].en, k_actions[i].pt));
            ImGui::TableNextColumn();
            bool waiting = cap_dev == BIND_DEV_KEY && cap_slot == slot;
            char lbl[96];
            if (waiting) snprintf(lbl, sizeof lbl, "%s %d##k%d", T("Press a key...", "Aperte a tecla..."), (ms_left + 999) / 1000, slot);
            else snprintf(lbl, sizeof lbl, "%s##k%d", key_name(g_bind_key[slot]), slot);
            int pushed = 0;
            if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
            if (!bind_is_default_slot(BIND_DEV_KEY, slot)) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1)); pushed++; }
            if (ImGui::Button(lbl, ImVec2(230, 0))) {
                if (waiting) bind_capture_cancel();
                else bind_capture_begin(BIND_DEV_KEY, slot);
            }
            ImGui::PopStyleColor(pushed);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    if (cap_dev == BIND_DEV_KEY)
        ImGui::TextDisabled("%s", T("Press the new key. Esc cancels; a key already in use swaps over.",
                                    "Aperte a nova tecla. Esc cancela; uma tecla já usada troca de lugar."));
    if (!bind_is_default(BIND_DEV_KEY) && ImGui::Button(T("Restore default keys", "Restaurar teclas padrão"))) bind_reset(BIND_DEV_KEY);
    ImGui::TextUnformatted(T("Mouse while aiming a Poké Ball: move = aim, left = throw.",
                             "Mouse na mira da Poké Ball: mover = mirar, esquerdo = arremessar."));
    ImGui::TextUnformatted(T("Otherwise: left = touch screen (Play with your partner). Right = B.",
                             "Fora dela: esquerdo = tocar na tela (Brincar com o parceiro). Direito = B."));
    ImGui::PushItemWidth(420);
    ImGui::SliderFloat(T("Mouse sensitivity", "Sensibilidade do mouse"), &g_settings.mouse_sens, 0.02f, 0.30f, "%.2f");
    ImGui::SliderFloat(T("Controller stick dead zone", "Zona morta do analógico"), &g_settings.stick_deadzone, 0.0f, 0.40f, "%.2f");
    ImGui::PopItemWidth();
}

// ------------------------------------------------------------------ file dialogs (SDL calls back on any thread)
static pthread_mutex_t g_pick_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_pick_game[1024], g_pick_mod[1024];
static void SDLCALL picked_game(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_pick_lock);
    snprintf(g_pick_game, sizeof g_pick_game, "%s", files[0]);
    pthread_mutex_unlock(&g_pick_lock);
}
static void SDLCALL picked_mod(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_pick_lock);
    snprintf(g_pick_mod, sizeof g_pick_mod, "%s", files[0]);
    pthread_mutex_unlock(&g_pick_lock);
}
static bool take(char *slot, char *out, size_t n) {
    pthread_mutex_lock(&g_pick_lock);
    bool got = slot[0] != 0;
    if (got) snprintf(out, n, "%s", slot), slot[0] = 0;
    pthread_mutex_unlock(&g_pick_lock);
    return got;
}

// ------------------------------------------------------------------ the game files section
static const ImVec4 k_green(0.45f, 0.85f, 0.50f, 1), k_red(1.0f, 0.45f, 0.40f, 1);

static void game_files_section(SDL_Window *win) {
    ImGui::SeparatorText(T("Game files", "Arquivos do jogo"));
    int st = setup_state();
    if (setup_running()) {
        static const char *const en[] = {"", "Looking for the game files...", "Copying the game files...",
                                         "Building the game data image...", "Reading the game code...",
                                         "Recompiling the game code...", "Finishing..."};
        static const char *const pt[] = {"", "Procurando os arquivos do jogo...", "Copiando os arquivos do jogo...",
                                         "Montando a imagem de dados do jogo...", "Lendo o código do jogo...",
                                         "Recompilando o código do jogo...", "Finalizando..."};
        ImGui::TextUnformatted(st >= 1 && st <= 6 ? T(en[st], pt[st]) : "...");
        ImGui::ProgressBar(setup_progress(), ImVec2(-FLT_MIN, 0));
        ImGui::TextDisabled("%s", T("This takes a while (10-30 minutes), only once. The game can be played when it's done.",
                                    "Isso demora (de 10 a 30 minutos), só uma vez. O jogo fica liberado quando terminar."));
        return;
    }
    bool ready = setup_ready();
    if (ready) {
        const char *name = game_name("extracted/exefs");
        ImGui::TextColored(k_green, g_pt ? "Pokémon: Let's Go, %s! - pronto para jogar" : "Pokémon: Let's Go, %s! - ready to play",
                           name ? name : "Eevee");
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Change...", "Trocar..."))) SDL_ShowOpenFolderDialog(picked_game, NULL, win, NULL, false);
        return;
    }
    if (st >= SETUP_FAIL_NOT_FOUND) {
        static const char *const en[] = {"No game files found in that folder.", "Those are packed game files (.xci / .nsp).",
                                         "That's not Pokémon: Let's Go, Eevee! or Pikachu!.", "The files couldn't be copied.",
                                         "The game code didn't build.", "The game code builder is missing from this copy."};
        static const char *const pt[] = {"Nenhum arquivo do jogo nessa pasta.", "Esses são arquivos do jogo empacotados (.xci / .nsp).",
                                         "Isso não é o Pokémon: Let's Go, Eevee! nem o Pikachu!.", "Os arquivos não puderam ser copiados.",
                                         "O código do jogo não compilou.", "O compilador do código do jogo não está nesta cópia."};
        static const char *const en_h[] = {"Choose the folder that holds exefs and romfs.", "Extract them first (exefs and romfs).",
                                           "Pick the folder of your own extracted Let's Go, Eevee! or Pikachu!.", "Check there are about 6 GB free and try again.",
                                           "Check there are about 2 GB free and try again; details in lasrecomp.log.", "Download the release again."};
        static const char *const pt_h[] = {"Escolha a pasta que tem exefs e romfs.", "Extraia antes (exefs e romfs).",
                                           "Escolha a pasta do seu Let's Go, Eevee! ou Pikachu! extraído.", "Confira se há uns 6 GB livres e tente de novo.",
                                           "Confira se há uns 2 GB livres e tente de novo; detalhes no lasrecomp.log.", "Baixe a release de novo."};
        int k = st - SETUP_FAIL_NOT_FOUND;
        ImGui::TextColored(k_red, "%s", T(en[k], pt[k]));
        ImGui::TextDisabled("%s", T(en_h[k], pt_h[k]));
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 900);
        ImGui::TextUnformatted(T("Select the folder with your own extracted Pokémon: Let's Go, Eevee! or Pikachu! (exefs and romfs).",
                                 "Selecione a pasta com o seu Pokémon: Let's Go, Eevee! ou Pikachu! extraído (exefs e romfs)."));
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Button(T("Select game folder...", "Selecionar pasta do jogo..."), ImVec2(-FLT_MIN, 0)))
        SDL_ShowOpenFolderDialog(picked_game, NULL, win, NULL, false);
}

// ------------------------------------------------------------------ the start screen
extern "C" void launcher_lg_run(const char *root) {
    (void)root;
    pick_language();
    ui_window_init();
    SDL_Window *win = ui_window();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // (the in-game menu's, when back here after a reboot)
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    static const ImWchar ranges[] = {0x20, 0xFF, 0};  // (Latin-1: Portuguese accents)
#ifdef __ANDROID__
    // No C:/Windows/Fonts on Android; AddFontFromFileTTF asserts on missing
    // file before returning null, so skip it entirely here.
    io.Fonts->AddFontDefault();
#else
    if (!io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 26, NULL, ranges)) io.Fonts->AddFontDefault();
#endif
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.5f);
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();

    static const char *const res_en[] = {"Automatic (window size)", "720p", "1080p", "1440p", "4K"};
    static const char *const res_pt[] = {"Automática (tamanho da janela)", "720p", "1080p", "1440p", "4K"};
    static const int res_lines[] = {0, 720, 1080, 1440, 2160};
    int res = 0;
    for (int i = 0; i < 5; i++) if (res_lines[i] == g_settings.res_height) res = i;
    bool full = g_settings.fullscreen, smaa = g_settings.antialias == AA_SMAA, fps60 = g_settings.fps == 60,
         wildb = g_settings.wild_battle;
    // Translation: the folders under mods/ (runtime/mods.h); 0 = none (the game's own English)
    static char mods[MODS_MAX][128];
    int nmods = mods_list(mods, MODS_MAX), mod = 0;
    for (int i = 0; i < nmods; i++) if (!strcmp(mods[i], g_settings.mod)) mod = i + 1;
    long shot_at = getenv("LARECOMP_LAUNCHER_SHOT") ? atol(getenv("LARECOMP_LAUNCHER_SHOT")) : 0, frame = 0;
    bool autoplay = getenv("LARECOMP_LAUNCHER_AUTOPLAY") != NULL, play = false;
    const char *keys_test = getenv("LARECOMP_LAUNCHER_KEYS");
    if (keys_test && atoi(keys_test) > 0) bind_capture_begin(BIND_DEV_KEY, atoi(keys_test));
    if (getenv("LARECOMP_SETUP_DIR")) setup_start(getenv("LARECOMP_SETUP_DIR"));
    if (getenv("LARECOMP_MOD_INSTALL")) mod_install_start(getenv("LARECOMP_MOD_INSTALL"));
    static const float bg[4] = {0.08f, 0.08f, 0.10f, 1};
    static const SDL_DialogFileFilter mod_filters[] = {{"Translation / mod (.zip, .rar, .7z)", "zip;rar;7z"}};

    while (!play) {
        menu_apply_window_requests(win);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (bind_capture_event(&e)) continue;  // (waiting for a new key: the press is the binding's)
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
        }
        char path[1024];
        if (take(g_pick_game, path, sizeof path)) setup_start(path);
        if (take(g_pick_mod, path, sizeof path)) mod_install_start(path);
        static int last_install;
        int inst = mod_install_state();
        if (inst != last_install && inst == MOD_INSTALL_DONE) {  // a new translation: listed and chosen
            nmods = mods_list(mods, MODS_MAX);
            for (int i = 0; i < nmods; i++) if (!strcmp(mods[i], mod_install_name())) mod = i + 1;
        }
        last_install = inst;
        bool ready = !setup_running() && setup_ready();
        if (autoplay && ready && inst != MOD_INSTALL_RUNNING) play = true;
        if (!vk_frame_begin(bg)) { SDL_Delay(16); continue; }
        if (keys_test && strchr(keys_test, ':') && frame == 5) {  // "<slot>:<key name>": the key pressed for the test
            SDL_Event k = {};
            k.type = SDL_EVENT_KEY_DOWN;
            k.key.scancode = SDL_GetScancodeFromName(strchr(keys_test, ':') + 1);
            k.key.down = true;
            SDL_PushEvent(&k);
        }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin(RT_APP_NAME, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings);
        game_files_section(win);
        ImGui::SeparatorText(T("Settings", "Configurações"));
        ImGui::PushItemWidth(420);
        ImGui::Combo(T("Resolution", "Resolução"), &res, g_pt ? res_pt : res_en, 5);
        if (ImGui::Checkbox(T("Fullscreen", "Tela cheia"), &full)) SDL_SetWindowFullscreen(win, full);
        ImGui::Checkbox(T("Anti-aliasing (SMAA)", "Suavizar serrilhado (SMAA)"), &smaa);
        // (both hook game functions by address: builds without address tables keep them off, runtime/fnhook.c)
        bool known = !setup_ready() || game_build("extracted/exefs") != GAME_BUILD_OTHER;
        const char *only = T("Only with version 1.0.0 (the base game) of Eevee or Pikachu for now.",
                             "Por enquanto só na versão 1.0.0 (o jogo base) do Eevee ou do Pikachu.");
        ImGui::BeginDisabled(!known);
        ImGui::Checkbox(T("60 fps (experimental)", "60 fps (experimental)"), &fps60);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", !known ? only : T("The world at 60 fps; menus and the Pokédex smoothed; battles and captures stay at 30.",
                                                     "Mundo a 60 fps; menus e Pokédex suavizados; batalhas e capturas continuam a 30."));
        ImGui::Checkbox(T("Wild battles (experimental)", "Batalhas selvagens (experimental)"), &wildb);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", !known ? only : T("Touching a wild Pokémon starts a classic battle, then the capture, instead of going straight to the capture.",
                                                     "Encostar num Pokémon selvagem começa uma batalha clássica e, depois, a captura, em vez de ir direto para a captura."));
        ImGui::EndDisabled();
        const char *none = T("None (English)", "Nenhuma (inglês)");
        if (ImGui::BeginCombo(T("Translation", "Tradução"), mod ? mods[mod - 1] : none)) {
            if (ImGui::Selectable(none, mod == 0)) mod = 0;
            for (int i = 0; i < nmods; i++)
                if (ImGui::Selectable(mods[i], mod == i + 1)) mod = i + 1;
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        if (inst == MOD_INSTALL_RUNNING) ImGui::TextDisabled("%s", T("Installing the translation...", "Instalando a tradução..."));
        else {
            if (ImGui::SmallButton(T("Install a translation...", "Instalar tradução...")))
                SDL_ShowOpenFileDialog(picked_mod, NULL, win, mod_filters, 1, NULL, false);
            if (inst == MOD_INSTALL_FAIL_UNPACK || inst == MOD_INSTALL_FAIL_NOT_MOD) {
                ImGui::SameLine();
                ImGui::TextColored(k_red, "%s", inst == MOD_INSTALL_FAIL_UNPACK ? T("Couldn't unpack that file.", "Não foi possível abrir esse arquivo.")
                                                                                : T("No translation for this game in that file.", "Esse arquivo não tem tradução para este jogo."));
            }
        }
        if (keys_test) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(T("Keyboard and mouse", "Teclado e mouse"))) keys_section();
        ImGui::Spacing();
        ImGui::BeginDisabled(!ready);
        if (frame == 0 && ready) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(T("Play", "Jogar"), ImVec2(-FLT_MIN, 0))) play = true;
        ImGui::EndDisabled();
        ImGui::TextDisabled("v" RT_APP_VERSION);
        ImGui::End();

        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
        bool shot = shot_at && ++frame == shot_at;
        if (!shot_at) frame++;
        vk_frame_end(shot ? (getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png") : NULL);
        if (shot) exit(0);
    }
    g_settings.res_height = res_lines[res];
    g_settings.fullscreen = full;
    g_settings.antialias = smaa ? AA_SMAA : AA_OFF;
    g_settings.fps = fps60 ? 60 : 30;
    g_settings.wild_battle = wildb;
    snprintf(g_settings.mod, sizeof g_settings.mod, "%s", mod ? mods[mod - 1] : "");
    settings_save();
    vk_imgui_backend_shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
}
