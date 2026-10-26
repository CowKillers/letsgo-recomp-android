# Relatório de Alterações — Let's Go Recomp p/ Android

**Projeto:** Port Android do `raulzrrs/letsgo-recomp` (recompilação estática de Pokémon Let's Go Eevee/Pikachu Switch → ARM64)
**Aparelho-alvo:** Galaxy S23 (Adreno 740, Android)
**Diretório do projeto:** `~/workspace/letsgo-port/`
**Última build:** v1350 (versionCode 1792432047) — 19/10/2026 ~14:48 BRT
**APK atual no Drive:** `LetsGoRecomp-v1350.apk` (pasta "Muse")
**Relatórios:** `reports/00_AUDITORIA.txt` até `06_VALIDACAO_FINAL.txt` (ORDEM MESTRE executada 19/10 ~14:40-14:52 BRT)

Cada item abaixo segue o formato: **erro → causa raiz → correção (arquivos/linhas) → build → verificação no aparelho**.

---

## Parte 1 — Correções iniciais (18/10 → 19/10, antes das builds numeradas)

### 1. `vkCreateDevice` falhava com erro -8
- **Erro:** Crash no boot com `vkCreateDevice` retornando -8 (`VK_ERROR_FEATURE_NOT_PRESENT`).
- **Causa raiz:** O código pedia ao driver Vulkan recursos 1.1/1.2/1.3 (ex.: `shaderInt64`) sem checar se o Adreno 740 os suportava.
- **Correção:** `src/runtime/nvn/vulkan/device.c` (linhas ~169–172): antes de criar o device, consulta as features suportadas via `vkGetPhysicalDeviceFeatures2` (cadeia `VkPhysicalDeviceVulkan11/12/13Features`) e só habilita as que existem. `shaderInt64` segue ausente, mas vira aviso em vez de falha fatal.
- **Verificação:** ✅ Log do aparelho: `[vk] gpu 0: Adreno (TM) 740 (api 1.3.128)` + `[vk] using Adreno (TM) 740`, device criado.

### 2. Assert fatal do ImGui ao carregar fonte do Windows
- **Erro:** Crash no boot: o launcher ImGui tentava carregar `C:/Windows/Fonts/segoeui.ttf` (caminho que não existe no Android); `AddFontFromFileTTF` dava assert antes de retornar null.
- **Causa raiz:** Código do launcher de PC usado sem adaptação.
- **Correção:** `src/runtime/ui/launcher_lg.mm` (linhas 208–214): o carregamento da fonte foi envolvido em `#ifdef __ANDROID__` → no Android usa `io.Fonts->AddFontDefault()`.
- **Verificação:** ✅ Jogo passou do boot no aparelho.

### 3. Pasta do jogo não chegava ao código nativo
- **Erro:** O runtime não sabia onde estavam `exefs/` e `romfs.bin` (a pasta é escolhida no launcher via SAF).
- **Causa raiz:** `nativeSetup` não repassava o caminho resolvido.
- **Correção:** `src/runtime/platform/android.c` (~linhas 56–59): `nativeSetup` define `LARECOMP_SETUP_DIR` com o caminho resolvido da pasta escolhida.
- **Verificação:** ✅ Log: `[paths] data: /storage/emulated/0/Download/1DMP/Compressed`.

### 4. Launcher do PC removido
- **Erro/solicitação:** A Emily pediu para não usar o launcher ImGui do PC no Android.
- **Correção:** `src/runtime/platform/android.c` (~linha 63): `nativeSetup` define `LARECOMP_NO_LAUNCHER=1`. As opções (60 fps, SMAA, batalhas selvagens) foram para o launcher Android (`LetsGoLauncherActivity`), que grava `larecomp.ini`.
- **Verificação:** ✅ Launcher Android com botões Jogar / Selecionar pasta / Ver log.

### 5. `dlopen` falhava: `cannot locate symbol "g_fnprof"`
- **Erro:** `liblasgame.so` não carregava por símbolo indefinido.
- **Causa raiz:** A lib do jogo referencia símbolos do runtime (`liblasrecomp.so`) mas não estava linkada contra ele.
- **Correção:** Relink de `liblasgame.so` com `-Lbuild/apk-lib -llasrecomp` (DT_NEEDED). Confirmação: main 60380, sdk 31906, subsdk0 17984, subsdk1 13342 funções.
- **Verificação:** ✅ Log: `[gamelib] .../liblasgame.so: main, 60380 functions` etc.

### 6. Acesso aos arquivos bloqueado (scoped storage)
- **Erro:** O app não conseguia ler `exefs/`/`romfs` fora do seu sandbox.
- **Causa raiz:** Android 11+ exige `MANAGE_EXTERNAL_STORAGE` para pastas arbitrárias.
- **Correção:** Permissões no `AndroidManifest.xml` + pedido em tempo de execução no launcher.
- **Verificação:** ✅ Jogo lê os dados da pasta escolhida.

### 7. `romfs.bin` ausente travava o boot
- **Erro:** O jogo exige o ARQUIVO `extracted/romfs.bin` (4,2 GB), não a pasta; sem ele, o boot parava.
- **Causa raiz:** `fs.c` faz `snprintf(path, "%s.bin", g_romfs_dir)`.
- **Correção:** Novo `src/runtime/romfs_build.c/.h`; `main.c` (~linha 146) chama `romfs_build()` no boot quando o `.bin` não existe mas a pasta `romfs/` existe. Construção leva ~3 min na 1ª vez.
- **Verificação:** ✅ Log: `[fs] romfs index: 27821 files` após a construção.

### 8. FATAL `no usable system font substitute`
- **Erro:** Crash: nenhuma fonte do sistema encontrada.
- **Causa raiz:** `pl.c` só procurava caminhos de Linux/Windows.
- **Correção:** `src/runtime/hle/pl.c` (~linhas 130–131): adicionados `/system/fonts/NotoSans-Regular.ttf`, `/system/fonts/Roboto-Regular.ttf`, `/system/fonts/DroidSans.ttf`.
- **Verificação:** ✅ Log: `[pl] system font /system/fonts/Roboto-Regular.ttf (2371712 bytes)`.

### 9. FATAL `needs FFmpeg's Opus decoder`
- **Erro:** Crash ao abrir áudio: decoder Opus ausente.
- **Causa raiz:** O FFmpeg pré-compilado não tinha `CONFIG_OPUS_DECODER`; além disso, `llvm-strip --strip-unneeded` REMOVIA o símbolo `ff_opus_decoder`.
- **Correção:** FFmpeg recompilado do zero (`~/workspace/letsgo-port/deps/ffmpeg`, NDK 28.2.13676358, `aarch64-linux-android26-clang`) com configure limpo (sem `--disable-everything`, que não ativava o decoder). **Lição:** nunca stripar `libavcodec.so`.
- **Verificação:** ✅ Log: `[hwopus] decoder opened: 48000 Hz, 1 ch`, sem crash.

### 10. Controles touch via JNI
- **Erro:** O overlay mandava `KeyEvent`s que o SDL não traduzia para gamepad — toques não moviam o jogo.
- **Causa raiz (tentativa 1):** `pad_buttons` é `static _Atomic` em `input.c` — `extern` de outro arquivo deu `UnsatisfiedLinkError`.
- **Correção:** `src/runtime/hle/input.c` (~linha 939): função não-estática `android_set_pad_button(int button, int pressed)`; JNI `Java_com_letsgorecomp_LetsGoActivity_nativeSetButton` em `src/runtime/platform/android.c` (~linha 324) a chama. `GamepadOverlay.sendKey()` mapeia keycodes Android → bits do Switch (A=0, B=1, X=2, Y=3, L=6, R=7, Plus=10, Minus=11, Left=12, Up=13, Right=14, Down=15) e chama o nativo (KeyEvent mantido como fallback). Analógicos: `nativeSetStick(stick, x, y)` → `pad_lx/ly/rx/ry`.
- **Causa raiz (tentativa 2):** O poll do SDL faz `pad_buttons = b` a cada frame, sobrescrevendo o JNI.
- **Correção:** Criado `_Atomic uint64_t touch_buttons` separado (input.c:30); JNI escreve nele; `fill_state()` faz OR de `pad_buttons | touch_buttons` (linhas 519, 531, 661).
- **Verificação:** ✅ Screenshots dela 19/10 07:49–07:52: avançou pela seleção de controle, intro do Prof. Carvalho, criação de personagem e gameplay (cena do laboratório).

### 11. Suporte a controle externo (Gamesir X3)
- **Solicitação:** Ela pediu suporte ao Gamesir X3.
- **Correção:** Nenhuma mudança necessária — `input.c` já abre gamepads via `SDL_GetGamepads`/`SDL_OpenGamepad` (log `[input] gamepad: <nome>`). Funciona pelo caminho `pad_buttons`.
- **Verificação:** ⚠️ Sem confirmação explícita dela de que o X3 funcionou.

### 12. Fonte DejaVuSans embarcada no APK
- **Erro/precaução:** Garantir fonte com acentos PT-BR mesmo sem fonte do sistema.
- **Correção:** `DejaVuSans.ttf` (742 KB) em `apk/assets/fonts/`; `LetsGoActivity.onCreate` copia para `<gameDir>/fonts/DejaVuSans.ttf` no 1º run; `pl.c` (~linha 115) checa `$LARECOMP_DATA_ROOT/fonts/DejaVuSans.ttf` antes das fontes do sistema. Auditoria confirmou 5.918 glyphs incluindo todos os acentos PT-BR.
- **Verificação:** ✅ Tela de seleção de idioma renderiza texto (prova que fonte não é o problema dos diálogos).

### 13. Idioma padrão revertido `pt-BR` → `en-US`
- **Erro:** Caixas de diálogo vazias no jogo.
- **Causa raiz:** Eu tinha mudado `game_language()` para `"pt-BR"` a pedido dela — ERRADO. A tradução de fã SUBSTITUI os arquivos da pasta `English/` (não existe pasta `Portuguese/`); o jogo precisa pedir `en-US` para carregar o texto PT-BR. Com `pt-BR`, procurava pasta inexistente → falha silenciosa.
- **Correção:** `src/runtime/mods.c:117` de volta para `"en-US"` (`return g_settings.game_lang[0] ? g_settings.game_lang : "en-US";`).
- **Verificação:** ✅ Log: `[am] GetDesiredLanguage: en-US`. ⚠️ Diálogos em si ainda não confirmados em screenshot (ela não chegou a um diálogo nas builds recentes).

### 14. Botão LOG no overlay
- **Solicitação:** O export de log funcionava, mas não dava para acessar durante o gameplay.
- **Correção:** `GamepadOverlay.java`: botão "LOG" (topo-direita, 64×40dp, semitransparente) — despeja `logcat -d` em `Downloads/letsgo-log.txt` e abre intent de compartilhar.
- **Verificação:** ✅ Visível no screenshot dela 19/10 09:28 (canto superior direito).

### 15. Paisagem forçada via override de `setOrientationBis`
- **Erro:** `setRequestedOrientation()` no `onCreate`/`onResume` era sobrescrito por `SDLActivity.setOrientationBis()` (chamado do código nativo) → activity voltava para retrato.
- **Correção:** `LetsGoActivity.java`: override de `setOrientationBis()` chamando sempre `setRequestedOrientation(SCREEN_ORIENTATION_SENSOR_LANDSCAPE)`. `AndroidManifest.xml` também tem `screenOrientation="landscape"` (o SDL ignora na prática).
- **Verificação:** ✅ Screenshot 19/10 09:33: activity em paisagem.

### 16. Hint de orientação do SDL
- **Erro:** Cinturão e suspensórios para a orientação (o set em `nativeSetup` podia não pegar, pois o SDL ainda não tinha iniciado).
- **Correção:** `SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight")` em `src/runtime/platform/android.c:67` (`nativeSetup`) e depois movido para `src/runtime/nvn/vulkan/device.c:799` (`ui_window_init()`, DEPOIS do `SDL_Init`).
- **Verificação:** ⚠️ Indireta (activity em paisagem nos screenshots).

---

## Parte 2 — Histórico de builds 19/10 (com versionCode)

> **Lição aprendida 19/10 ~08:50:** o `versionCode` estava travado em `"1"` — o Android NÃO atualiza um app instalado por cima se o versionCode não aumenta. Desde então, cada build usa timestamp Unix como versionCode.

### v0938 — 09:38 (versionCode 1792413421)
- **Mudança:** Hint `SDL_HINT_ORIENTATIONS` adicionado no código nativo (cinturão e suspensórios da correção 15).
- **Verificação:** ⚠️ Ela não confirmou teste desta build (foi copiada para `LetsGoRecomp-v0938.apk` no Drive a pedido dela, para evitar confusão de cache).

### v0951 — 09:51 (versionCode 1792414284)
- **Erro:** Jogo 16:9 (ex.: 1920x1080) esticava na tela 19.5:9 do S23 (2340x1080).
- **Causa raiz:** O swapchain era criado no tamanho da superfície (2340x1080); o jogo esticava seu frame 16:9 para preencher.
- **Correção:** `device.c` `create_swapchain()`: força extent 16:9 (`target_w = height*16/9` → 1920x1080).
- **Verificação:** ✅ Log: `[vk] Adreno (TM) 740, swapchain 1920x1080`. ⚠️ Investigação posterior no projeto Wonder mostrou que o correto seria letterbox via present pass (não só forçar o extent) — ver Parte 3.

### v1001 — 10:01 (versionCode 1792414903)
- **Erro:** Contador de FPS invisível/não confirmado em nenhum screenshot (o `bringToFront()` não era confiável e o fundo semitransparente sumia no jogo claro).
- **Correção cirúrgica:** `GamepadOverlay.java`: o `fpsView` foi movido para o FIM do `build()` (adicionado por último = fica no topo naturalmente, sem `bringToFront`), fundo preto sólido, texto 18sp verde, texto inicial `"FPS --"`. Atualização a cada 500 ms via JNI `nativeGetFps()`/`nativeGetFrameMs()` lendo `g_current_fps`/`g_frame_ms` (`device.c:539–540, 760–761`).
- **Verificação:** ✅ Screenshot 10:09: `"30,4 fps | 34 ms"` verde no canto superior esquerdo.

### v1017 — 10:23 (versionCode 1792415869)
- **Erros:** (a) 1080p deixava o jogo lento (pedido dela: "tira o 1080p, deixa 720p normal"); (b) o hint de orientação setado em `nativeSetup` não pegava (SDL ainda não iniciado).
- **Correções:** `device.c` `create_swapchain()`: extent forçado para **1280x720** (720p 16:9, resolução handheld nativa do jogo). `SDL_HINT_ORIENTATIONS` movido para `ui_window_init()` DEPOIS do `SDL_Init` (`device.c:799`).
- **Verificação:** ✅ Log esperado: `swapchain 1280x720`. ⚠️ Screenshots 10:12–10:13 mostraram o conteúdo 3D GIRADO 90° (personagem deitado) — ver v1030.

### v1030 — 10:30 (versionCode 1792416627)
- **Erros:** (a) Conteúdo 3D renderizando girado 90° (720x1280 em vez de 1280x720); (b) barra de notificação aparecendo (jogo não fullscreen).
- **Causa raiz (a):** Hipótese de que `caps.currentTransform` aplicava rotação de 90°.
- **Correções:** `device.c:327`: `preTransform` forçado para `VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR`. `LetsGoActivity.java`: modo imersivo fullscreen (esconde status + navegação) no `onCreate`/`onResume`.
- **Verificação:** ❌ **A build TRAVOU no aparelho** — o driver não suporta forçar IDENTITY. Revertida na v1040. O fullscreen também não pegou (o `SDLActivity` resetava a visibilidade da UI depois do meu código).

### v1040 — 10:46 (versionCode 1792417205)
- **Erro:** v1030 travava (ver acima).
- **Correção:** `device.c:327` revertido para `.preTransform = caps.currentTransform`. Mantidos: fullscreen imersivo + swapchain 1280x720.
- **Verificação:** ✅ Jogo abre sem travar. ⚠️ Fullscreen ainda não efetivo (barra visível no screenshot 10:47).

### v1052 — 10:59 (versionCode 1792418019)
- **Erro:** Fullscreen não pegava porque o código rodava ANTES de `super.onCreate()` — o `SDLActivity` resetava a visibilidade da UI depois.
- **Correção:** `LetsGoActivity.java`: criado método `applyFullscreen()` (linhas 19–29); chamado DEPOIS de `super.onCreate()` (linha 57) e no `onResume()` (linha 72); adicionado `onWindowFocusChanged()` (linhas 77–80) que reaplica o fullscreen quando a janela reganha o foco.
- **Verificação:** ⚠️ Launcher ficou fullscreen, mas as barras voltavam ao abrir a LetsGoActivity — ver v1250 (causa raiz: o SDL nunca recebia `SDL_SetWindowFullscreen`).

### v1136 — 11:36 BRT (versionCode 1792420613) — ORDEM DE EXECUÇÃO dela
- **Escopo:** Ordem de execução escrita por ela mesma: corrigir orientação, apresentação Vulkan e textos ausentes (base v1052). Relatório completo em `RELATORIO-v1136.txt`.
- **Correções:**
  - `device.c` `create_swapchain()`: removido extent fixo 1280x720 — respeita `currentExtent` da superfície (ou tamanho da janela limitado a min/maxImageExtent quando variável). Adicionados logs de currentExtent, currentTransform, supportedTransforms, imageExtent e preTransform.
  - `vk.h`: novo campo `sc_rotation` no VkG; `device.c` mapeia `currentTransform` → rotação 0/90/180/270°.
  - `shaders.c` `present.vert`: compensação de rotação via `pc.a.y` aplicada aos UVs.
  - `device.c` `vk_frame_begin()`: recriação da swapchain só quando a janela realmente muda (last_w/last_h) — antes comparava tamanho todo frame.
  - `objects.c` `nvnTextureFlushTexels()`/`nvnTextureInvalidateTexels()`: implementados de verdade (eram vazios — `{ (void)c; }`) via `backend_pool_written(addr, size)` com dirty-tracking. **Hipótese forte para os textos ausentes:** o jogo atualiza o atlas de glifos na CPU e o Flush era ignorado → GPU exibia textura desatualizada (vazia).
  - `build-apk.sh`: `-A apk/assets` no link do aapt2 (fonte DejaVuSans agora vai para o APK — verificado presente).
  - `LetsGoActivity.java`: cópia da fonte com diagnóstico (Log.i/e/w), validação de tamanho (>100KB), limpeza de arquivo parcial.
  - `build-runtime.sh`: falha de compilação paralela agora via arquivo marcador `$OBJDIR/.build_failed` (a variável `fail` em subshells não afetava o pai).
  - Auditoria `shaderInt64`: aviso informativo, sem relação com os textos — nenhuma ação.
- **SHA256:** `633778d994017b8da99f4e92af3beacdc1608ac593d38f810ce2d6aabac70b57`
- **Verificação:** ✅ Compila sem erros (SMOKE_OK). ⚠️ Validação visual pendente no aparelho.

### v1155 — 11:55 BRT — NÃO PUBLICADA (a pedido dela)
- **Mudanças:** swapchain forçado 1920x1080 (16:9 via imageExtent) + fullscreen via tema (`styles.xml` `windowFullscreen=true`, como o Wonder).
- **Verificação:** ❌ Buildada da árvore antiga (sem o tema, por causa do restart do JDK) — ela ordenou **não subir**: "pra que eu vou querer esse apk inútil". Nunca foi ao Drive.

### v1216 — 12:17 BRT (SHA256 `f0fe01dd446859f435cb6715b0cf145f709f5c62469de4ab39fcc77996100f49`)
- **Mudanças:** fullscreen via TEMA (`styles.xml`, igual ao Wonder) + 16:9 forçado no swapchain (1920x1080, sem esticar).
- **Erro introduzido (REGRESSÃO):** `[post]` foi de `1280x720 smaa -> 1280x720` para `1920x1080 smaa -> 1920x1080` = **2,25x mais pixels** nos passes de pós-processamento (smaa_edge/weight/blend). Forçar 16:9 via `imageExtent` aumentou a resolução interna do jogo junto — hipótese levantada por ela a partir do log.
- **Verificação:** ❌ Gameplay caiu para ~20 FPS (relatado por ela). "E não resolveu" + "Continua esticado". Lição: **a proporção deve ser tratada no enquadramento (viewport), separada das dimensões da superfície/swapchain.**

### v1235 — 12:35 BRT — intermediária, superada antes do teste
- **Mudanças:** swapchain do tamanho da superfície (sem forçar 16:9 — letterbox no viewport) + fullscreen via `WindowInsetsController` (API 30+).
- **Verificação:** ⏳ Superada pela v1244 cinco minutos depois; sem teste no aparelho.

### v1244 — 12:44 BRT (versionCode 1792424263) — **BUILD ATUAL** — ordem dela à risca
- **Escopo:** "CORRIGIR REGRESSÃO DE DESEMPENHO E TELA CHEIA" — ordem dela executada à risca. Relatório completo em `RELATORIO-v1244.txt`.
- **Causa da regressão (comprovada pelo log):** a v1216 forçou 16:9 via `imageExtent` → `[post]` 1280x720 → 1920x1080 (2,25x pixels nos passes SMAA).
- **Correções:**
  - `device.c` `create_swapchain()`: REMOVIDO o 16:9 forçado — swapchain usa o `currentExtent` da superfície (2340x1080 no S23). O 16:9 é tratado por `output_scale()` via viewport (vx,vy,vw,vh) com letterbox, sem deformação.
  - `android.c:67` (`nativeSetup()`): `setenv("LARECOMP_SCALE", "0.6667", 1)` — renderização interna forçada em 720p; passes SMAA em 1280x720, upscale no passe final (present) para a superfície. Independente do tamanho do swapchain.
  - `LetsGoActivity.java` `applyFullscreen()` (linhas 19–41): substituído o método antigo (flags obsoletas, sobrescritas pelo SDLActivity) por `WindowInsetsController` — API 30+: `setDecorFitsSystemWindows(false)` + `hide(systemBars())` + `BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE`; fallback API 28–29. Aplicado após `super.onCreate()`, no `onResume()` e no `onWindowFocusChanged()`.
  - `apk/res/values/styles.xml` (NOVO) + `AndroidManifest.xml`: tema Fullscreen (`windowFullscreen=true`) na LetsGoActivity, igual ao projeto Wonder.
- **SHA256:** `f40b7b8df68dcc9569056c01686b3db6a6e80d739d9caa5006570fa8be5346c6`
- **Resoluções separadas:** interna 1280x720 → SMAA 1280x720 → saída 2340x1080 (upscale no present) → swapchain 2340x1080 (= superfície).
- **Verificação:** ⏳ Checklist pendente no aparelho: barras ocultas durante o jogo; funciona após minimizar/retomar; orientação correta sem distorção; `[post]` mostra 1280x720 → 2340x1080 no log; sem recriação contínua da swapchain; 30 FPS na mesma cena que dava 20.

### v1250 — 12:50 BRT — EM ANDAMENTO (build travada pelo restart da VM)
- **Erro:** O launcher fica fullscreen, mas ao abrir a LetsGoActivity as barras de status/navegação reaparecem.
- **Causa raiz identificada:** `src/runtime/settings.c:20` — `g_settings.fullscreen` default `false` → `device.c:856` nunca chamava `SDL_SetWindowFullscreen(window, true)` no Android → o SDLActivity nunca recebia o `COMMAND_CHANGE_WINDOW_STYLE` para esconder as barras. O `applyFullscreen()` da Activity brigava sozinho e perdia.
- **Correção aplicada no código:** `device.c:852-854` — no Android, chama `SDL_SetWindowFullscreen(window, true)` sempre, com log do retorno e `SDL_GetError()`; logging diagnóstico de insets e foco.
- **Verificação:** ⏳ JDK sumiu de novo (restart da VM ~12:51 BRT) — build pendente. O arquivo local `LetsGoRecomp-v1250.apk` existe mas **não foi validado** como contendo o fix — não subir sem rebuild.

---

## Parte 3 — Estado atual e pendências (19/10 ~12:52 BRT)

### Resolvido desde as 11:00
| Item | Evidência |
|---|---|
| Causa raiz da regressão de FPS | Log: `[post]` 1280x720 → 1920x1080 depois de forçar 16:9 via `imageExtent` (v1216); corrigido na v1244 |
| Separação de resoluções | Interna 1280x720 (`LARECOMP_SCALE=0.6667`) / SMAA 1280x720 / saída 2340x1080 / swapchain = superfície |
| Causa raiz do fullscreen perdido na LetsGoActivity | `settings.c:20` (`fullscreen=false`) → SDL nunca recebia `SDL_SetWindowFullscreen` (fix no código; build v1250 pendente) |
| Recriação contínua da swapchain | `vk_frame_begin()` recria só quando a janela muda (v1136) |
| Build que não falhava em erro de compilação paralela | `build-runtime.sh` falha via arquivo marcador (v1136) |
| Fonte não ia para o APK | `-A apk/assets` no aapt2 (v1136) |
| `nvnTextureFlushTexels` vazio (textos) | Implementado via dirty-tracking (v1136) — validação visual pendente |

### Funcionando (verificado no aparelho)
| Item | Evidência |
|---|---|
| Boot completo, Vulkan no Adreno 740 | Log: `swapchain`, `nvn device initialized` |
| 30 fps estáveis | FPS overlay + log `[vk] t=... 29.9 fps` |
| Controles touch | Screenshots de gameplay 07:49–07:52 |
| Botão LOG no overlay | Screenshot 09:28 |
| FPS counter visível | Screenshot 10:09: `30,4 fps \| 34 ms` |
| Idioma `en-US` (texto PT-BR da tradução) | Log: `GetDesiredLanguage: en-US` |
| Save lendo/escrevendo | Log: `[save] read/write save/save_0/savedata.bin` |
| Áudio Opus | Log: `[hwopus] decoder opened` |
| Fonte do sistema | Log: `[pl] system font .../Roboto-Regular.ttf` |
| romfs auto-build | Log: `[fs] romfs index: 27821 files` |

### Pendente / não confirmado
1. **Veredito da v1244 no aparelho** — checklist: barras ocultas durante o jogo; funciona após minimizar/retomar; orientação correta, imagem 16:9 sem distorção; `[post]` mostra `1280x720 smaa -> 2340x1080` no log; sem recriação contínua da swapchain; 30 FPS na mesma cena que dava 20.
2. **v1250 (fullscreen via SDL)** — build travada pelo restart da VM/JDK ~12:51 BRT; aguardando rebuild + teste.
3. **Texto nos diálogos** — fix `en-US` + `FlushTexels` implementado; ainda sem confirmação visual de letras em diálogo (ela não chegou a um diálogo nas builds recentes).
4. **Gamesir X3** — sem confirmação dela de funcionamento.
5. **Ultrawide 19.5:9** — ela pediu para deixar em 16:9 por enquanto ("depois fazemos um ultrawide").

### Roadmap (prioridade dela)
1. Veredito dela sobre a v1052 (fullscreen? diálogos?).
2. Letterbox 16:9 correto (copiar present pass do Wonder) — depois, ultrawide.
3. Shaders SPIR-V pré-compilados (como o Wonder) — elimina stutter de compilação em runtime.
4. Importador de NSP in-app (pedido dela 19/10 05:20; `apk/importer/` com `nxcrypto`, parser PFS0 parcial — faltam NCA, extração RomFS, JNI e UI).
5. Otimizações futuras discutidas: render scale no launcher → Turnip via adrenotools → FSR 1 (o Wonder TEM FSR 1 de verdade — ela estava certa).
6. 60 fps: DESCARTADO por acordo (jogo é 30 fps por design, lógica amarrada a 30 Hz).

---

## Anexo — Arquivos do Drive (pasta "Muse", ID `1Lqe1xCOjnKGXCMf85OJ_Z7u8dy_KFfjy`)
- `LetsGoRecomp-v1244.apk` + `RELATORIO-v1244.txt` — builds atuais
- `LetsGoRecomp-v1216.apk`, `v1136.apk` (+ `RELATORIO-v1136.txt`), `v1052`, `v1040`, `v1030`, `v1017`, `v1001`, `v0951`, `v0938`, `boot-test` — builds anteriores
- ⚠️ v1155 nunca foi publicada (buildada da árvore antiga sem o tema fullscreen — ela ordenou não subir); v1235 foi superada pela v1244 antes do teste
- `letsgo-exefs.zip` (21 MB), `letsgo-romfs.z01/z02/.zip` — dados extraídos do dump dela
- Pasta **"letsgo"**: `letsgo-port-project.zip` (51 MB, código-fonte + scripts, SEM a ROM — a pedido dela 19/10 11:05) + `letsgo-deps-part-aa/ab` (livrarias pré-compiladas: FFmpeg c/ Opus, SDL3, etc.) + `RELATORIO-ALTERACOES.md` (**este arquivo — cópia no Drive está na versão das 11:18, desatualizada; esta versão local cobre até a v1244**) + `RELATORIO-v1136.txt`

---

## ORDEM MESTRE — 19/10/2026 ~14:40-14:52 BRT

**Contexto:** Ordem formal de 7 etapas (0-6) para correção definitiva dos 3 defeitos graves (fullscreen, textos, performance).

### ETAPA 0 — Auditoria (`reports/00_AUDITORIA.txt`)
- Estrutura completa mapeada
- Causas raiz identificadas por inspeção de código

### ETAPA 1 — Fullscreen + 16:9 (`reports/01_FULLSCREEN_FIX.txt`)
- **v1250** (13:25): `SDL_SetWindowFullscreen(true)` incondicional no Android (`device.c`)
  - Causa raiz: `g_settings.fullscreen=false` (default em `settings.c:20`)
- **v1319** (13:45): Camada agressiva (FLAG_FULLSCREEN + cutout SHORT_EDGES + systemUiVisibility)
- **v1332** (13:52): SIMPLIFICADO — removido `applyFullscreen()` manual que conflitava com SDLActivity
  - Princípio: tema `@style/Fullscreen` + SDL apenas (igual Wonder)
- 16:9: viewport 1920x1080 centralizado (vx=210, vy=0) via `output_scale.c`
- Touch usa `g_present_rect` (viewport), mapeamento correto

### ETAPA 2 — Textos pl:u (`reports/02_TEXT_RENDERING_FIX.txt`)
- **Causa comprovada:** Jogo acessa `pl:u` via IPC, mas não havia handler registrado → stub genérico retornava só "ok" sem dados da fonte
- **Solução (v1350):**
  - `src/runtime/hle/pl.c`: adicionadas `pl_font_address()`, `pl_font_size()` públicas
  - `src/runtime/services/pl_u.c` (NOVO): `SERVICE("pl:u")` com cmds 0 (load state), 1 (size), 2 (address)
- Compilação: SUCESSO

### ETAPA 3 — Shaders (`reports/03_VULKAN_PIPELINES.txt`)
- Skip de draws é by design (async, como Metal)
- Cache SPIR-V existe (`cache/spirv/<hash>.spv`) mas chave não inclui GPU/driver
- Recomendação: pré-popular cache (requer coleta assistida)

### ETAPA 4 — Performance (`reports/04_PERFORMANCE.txt`)
- 30fps estáveis após warmup (wait 4-5ms, compile 0ms)
- 720p forçado via `LARECOMP_SCALE=0.6667` restaurou performance
- FSR1 ativo: `[post] 1280x720 smaa fsr1 -> 1920x1080`

### ETAPA 5 — Surface Recovery (`reports/05_SURFACE_RECOVERY.txt`)
- **Implementado (v1350):** `recover_lost_surface()` em `device.c`
  - Trata `VK_ERROR_SURFACE_LOST_KHR` em `vk_frame_begin()` e `vk_present()`
  - Destrói superfície perdida, recria via SDL, recria swapchain
- Compilação: SUCESSO

### ETAPA 6 — Entrega (`reports/06_VALIDACAO_FINAL.txt`)
- **v1350** (14:48): versionCode 1792432047, SHA256 `538d08ab2c29e16b6a1b149f5c50a465a1f4943c6d5555c6772c92559e1c3b20`
- 7 relatórios em `reports/`
- APK no Drive (pasta "Muse")

### Arquivos alterados na ORDEM MESTRE
- `src/runtime/nvn/vulkan/device.c` (SDL fullscreen, surface recovery, vk_sdl_window)
- `src/runtime/hle/pl.c` (pl_font_address, pl_font_size públicas)
- `src/runtime/services/pl_u.c` (NOVO — serviço pl:u IPC)
- `apk/java/com/letsgorecomp/LetsGoActivity.java` (applyFullscreen simplificado)
- `apk/AndroidManifest.xml` (versionCode)
- `reports/*.txt` (7 arquivos novos)
