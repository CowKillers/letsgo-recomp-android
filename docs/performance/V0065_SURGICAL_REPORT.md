# Relatório v0065 — ORDEM CIRÚRGICA (Continuidade v0063)

**Data:** 2026-10-21
**Build:** v0065 (versionCode 1792568973)
**Base:** v0063 (reversível, sem V-flip)

---

## MISSÃO A — CORRIGIR O CRASH VULKAN ✅ IMPLEMENTADA

### Problema
`surface recovery FAILED: Android native window is not available` seguida de
`FATAL: vkGetPhysicalDeviceSurfaceCapabilitiesKHR ... -1000000000`
Backtrace: `vk_frame_begin` → `backend_present`.

A correção anterior falhou porque `recreate_swapchain()` chamava `create_swapchain()`
diretamente sem verificar a validade da superfície.

### Arquivos Alterados

**`src/runtime/nvn/vulkan/vk.h`:**
- Adicionados defines: `PRESENT_READY` (0), `PRESENT_SURFACE_LOST` (1),
  `PRESENT_WAITING_FOR_WINDOW` (2), `PRESENT_RECREATING` (3)
- Adicionado campo `int present_state` ao struct `VkG`

**`src/runtime/nvn/vulkan/device.c`:**

1. **`create_swapchain()`** (linha ~695):
   - Verifica `g_vk.present_state` antes de prosseguir
   - Retorna early se não estiver READY/RECREATING
   - Verifica `g_vk.surface == VK_NULL_HANDLE` → seta WAITING_FOR_WINDOW
   - Substitui `VK_CHECK` por verificação manual de `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`
   - Em falha: seta SURFACE_LOST, retorna sem FATAL
   - Verifica `vk_sdl_window()` antes de usar

2. **`recreate_swapchain()`** (linha ~793):
   - Retorna early se `present_state == PRESENT_WAITING_FOR_WINDOW`
   - Seta RECREATING antes de `vkDeviceWaitIdle`
   - Após `create_swapchain()`, se ainda em RECREATING e swapchain válido → READY

3. **`recover_lost_surface()`** (linha ~814):
   - Seta SURFACE_LOST no início
   - Em falha de `SDL_Vulkan_CreateSurface`: seta WAITING_FOR_WINDOW
   - Em sucesso: seta RECREATING, chama `create_swapchain()`
   - Se swapchain criado: seta READY + log "surface recovery: READY"

4. **`vk_frame_begin()`** (linha ~896):
   - Verifica `g_vk.present_state != PRESENT_READY` no início
   - Se SURFACE_LOST: tenta `recover_lost_surface()` no máximo 1x a cada 60 frames
   - Se ainda não READY: retorna `false` (pula o frame, não faz acquire/present)
   - Obtém `window` via `vk_sdl_window()` (não usa global potencialmente inválido)

5. **Inicialização** (linha ~1425):
   - Seta `g_vk.present_state = PRESENT_READY` após criar superfície inicial

### Comportamento
- `VK_ERROR_SURFACE_LOST_KHR` → `recover_lost_surface()` → máquina de estados
- `VK_ERROR_OUT_OF_DATE_KHR` → `recreate_swapchain()` → verifica estado
- Sem ANativeWindow: estado WAITING_FOR_WINDOW, frames pulados, sem FATAL
- Sem retry em loop apertado: máximo 1 tentativa a cada 60 frames

### Não implementado (requer teste no dispositivo)
- Testes simulando perda/suspensão/destruição/restauração
- Proteção contra acesso concorrente (thread de render vs thread de janela)
  - O código atual assume single-thread para apresentação, como o original

---

## MISSÃO B — INVESTIGAR SHADER IDENTIFICADO ✅ INSTRUMENTAÇÃO PRONTA

### Hash Fornecido
`fshash=7270eb69fbecccee` — aparece nos draws do atlas 2048×1024 nos frames iniciais.

### Limitação
O fshash é calculado do código Maxwell guest (`hmix` sobre 0x200 bytes em `fs+0x30`),
não do GLSL. Não é reversível para localizar o shader sem o cache do dispositivo.

### Arquivos Alterados

**`src/runtime/nvn/vulkan/shaders.c`:**
- **`vk_glsl_to_spirv()`**: adicionado dump de GLSL quando `LARECOMP_DUMP_FONT_GLSL=1`
  - Salva fragment shaders (stage 1) em `$LARECOMP_DATA_ROOT/cache/glsl_<hash>.frag`
  - Usa `fopen(path, "wx")` para não sobrescrever existentes
  - Permite análise offline do shader real usado para texto

**`src/runtime/nvn/vulkan/exec.c`:**
- **`[font-draw]`**: agora inclui `fshash=%016llx` para correlação

### Como Usar
1. No app, setar `LARECOMP_DUMP_FONT_GLSL=1` (via `Os.setenv` no Java ou adb)
2. Abrir um diálogo no jogo
3. Os arquivos `glsl_*.frag` serão criados em `<data_root>/cache/`
4. Enviar os arquivos para análise do código de amostragem R8

### Análise Pendente (requer os arquivos GLSL do dispositivo)
- Tipo/formato efetivo da imagem Vulkan (confirmar R8_UNORM)
- Image view e component swizzle
- Descriptor e sampler utilizados
- UV e região do atlas amostrada
- Saída RGBA do fragment shader
- Valor final de alpha
- Blend, depth, stencil, scissor

---

## MISSÃO C — INSTRUMENTAÇÃO FUNCIONAL ✅ IMPLEMENTADA

### Problema
Log tinha 10 `[font-draw]` mas sem dumps de UV/RGBA/alpha/framebuffer.
Capturava apenas os 10 primeiros draws após inicialização, não uma cena de diálogo real.

### Arquivos Alterados

**`src/runtime/nvn/vulkan/exec.c`:**
- **`[font-draw]`**: adicionado gatilho de captura sob demanda
  - Verifica arquivo `$LARECOMP_DATA_ROOT/capture_font` a cada draw
  - Se arquivo existe: ativa `capture_triggered=1`, remove o arquivo, loga `[font-capture] TRIGGERED`
  - Captura o próximo draw com atlas (além dos 10 iniciais)
  - Loga `[font-capture] CAPTURED` e marca `[TRIGGERED]` no `[font-draw]`
  - Permite capturar uma cena de diálogo real sob demanda

### Como Usar
1. Abrir um diálogo no jogo (NPC)
2. Via adb: `adb shell "touch /storage/emulated/0/Download/1DMP/Compressed/capture_font"`
   (ajustar o caminho para o `LARECOMP_DATA_ROOT` real)
3. O próximo draw com o atlas será logado com `[TRIGGERED]`
4. O logcat permanece legível (sem dump de milhares de linhas)

### Não implementado
- Dump de UV/RGBA/alpha em arquivo (requer instrumentação de shader mais profunda)
- Captura de framebuffer antes/depois (requer `vkCmdCopyImage` + save PNG)
  - Estes podem ser adicionados na próxima iteração se o GLSL não revelar a causa

---

## Arquivos Modificados (Resumo)

| Arquivo | Funções | Missão |
|---------|---------|--------|
| `src/runtime/nvn/vulkan/vk.h` | Struct `VkG` (+`present_state`), defines `PRESENT_*` | A |
| `src/runtime/nvn/vulkan/device.c` | `create_swapchain()`, `recreate_swapchain()`, `recover_lost_surface()`, `vk_frame_begin()`, init | A |
| `src/runtime/nvn/vulkan/shaders.c` | `vk_glsl_to_spirv()` (+dump GLSL) | B |
| `src/runtime/nvn/vulkan/exec.c` | Bloco `[font-draw]` (+fshash, +gatilho) | B, C |
| `apk/java/com/letsgorecomp/LetsGoActivity.java` | (sem alteração nesta build — V-flip já removido na v0063) | — |
| `apk/AndroidManifest.xml` | `versionCode` 1792568973 | — |

---

## Testes

### Compilação
- ✅ `SMOKE_OK` (65 objetos)
- ✅ `RUNTIME_LINK_OK`
- ✅ `APK_OK` (373 MB)
- ✅ Assinatura SHA-256 `3ee8af859b20ffade872084aedf1ca732f05240f4b7226dfc8beff19a0fa8248`

### Não testados (requer dispositivo)
- Perda real de superfície (background/resume)
- Recuperação com/sem janela válida
- Captura sob demanda do diálogo
- Dump de GLSL do shader de texto

---

## Próximos Passos

1. **Testar v0065 no S23**: verificar se o crash de superfície foi eliminado
2. **Captura sob demanda**: abrir diálogo, criar arquivo `capture_font`, enviar log com `[TRIGGERED]`
3. **Dump GLSL**: setar `LARECOMP_DUMP_FONT_GLSL=1`, abrir diálogo, enviar arquivos `glsl_*.frag`
4. **Análise**: com o GLSL em mãos, identificar como o shader amostra o R8 e calcula o alpha
5. **Correção cirúrgica**: modificar apenas o pipeline de texto, se a causa for confirmada

---

## Preservação

- v0063 mantida como referência reversível (sem máquina de estados, sem dump GLSL)
- Nenhuma alteração em Beluga, mensagens, ou ROM
- V-flip NÃO reintroduzido
- Gameplay, áudio, controles, e desempenho preservados
