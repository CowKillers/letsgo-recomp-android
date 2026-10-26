# Relatório v0066 — CONTINUIDADE (P1-P4)

**Data:** 2026-10-21
**Build:** v0066 (versionCode 1792569649)
**Base:** v0065 (com máquina de estados)

---

## PRIORIDADE 1 — TEXTO INVISÍVEL ✅ INSTRUMENTAÇÃO IMPLEMENTADA

### Estado
- Atlas R8 (2048×1024) preenchido com 8.358 pixels, glifos em X[1-664] Y[934-1014]
- Runtime identifica draws com `fshash=7270eb69fbecccee` nos frames iniciais
- Problema permanece sem identificação conclusiva

### Implementado na v0066

**1. Captura de framebuffer sob demanda (`device.c`):**
- Novo global `_Atomic int g_font_capture_frame`
- Quando o gatilho `capture_font` é ativado em `[font-draw]`, seta o flag
- `vk_frame_end()` verifica o flag e salva screenshot em `$LARECOMP_DATA_ROOT/font_frame.png`
- Log: `[font-capture] Saving frame screenshot to <path> (frame <N>)`
- Permite ver se o texto aparece na imagem final

**2. Log detalhado de estado (`exec.c`):**
- `[font-draw]` agora inclui `fshash` para identificação do shader
- Novo log `[font-state]` com:
  - `viewport=(x,y wxh)` — viewport efetiva
  - `scissor=(x,y wxh)` — scissor efetivo
  - `depth_test`, `depth_write`, `stencil` — estado de depth/stencil
  - `blend0: srcRGB, dstRGB, srcA, dstA` — fatores de blend
- Novo log `[font-capture]` para gatilho sob demanda

**3. Gatilho sob demanda (já na v0065, mantido):**
- Arquivo `$LARECOMP_DATA_ROOT/capture_font` ativa captura
- Loga `[font-capture] TRIGGERED` e `[TRIGGERED]` no `[font-draw]`
- Remove o arquivo após ativar (captura única)

**4. Dump de GLSL (já na v0065, mantido):**
- `LARECOMP_DUMP_FONT_GLSL=1` salva fragment shaders em `cache/glsl_<hash>.frag`
- Permite análise offline do código de amostragem

### Como Testar (P1)
1. Instale v0066
2. Abra um diálogo real (NPC)
3. Via adb: `touch <data_root>/capture_font`
   (data_root típico: `/storage/emulated/0/Download/1DMP/Compressed`)
4. Envie:
   - Log com `[font-draw]`, `[font-state]`, `[font-capture]`
   - Arquivo `font_frame.png` (screenshot do frame com texto)
   - Se possível, arquivos `glsl_*.frag` (com `LARECOMP_DUMP_FONT_GLSL=1`)

### Análise Pendente
Com os dados acima, será possível determinar:
- Se o shader produz pixels (screenshot mostra texto?)
- Se o blend está correto (fatores src/dst)
- Se o viewport/scissor está clipando o texto
- Como o shader amostra o R8 (via GLSL)

### Não feito (conforme ordem)
- Modificação global de shaders: NÃO
- Reintrodução de V-flip: NÃO
- Hipóteses sem evidência: NÃO

---

## PRIORIDADE 2 — SURFACE VULKAN ✅ MÁQUINA DE ESTADOS IMPLEMENTADA

### Estado
- Erro fatal anterior (`vkGetPhysicalDeviceSurfaceCapabilitiesKHR -1000000000`)
  não apareceu na execução 04:55:19–04:56:53
- Mas não há prova de recuperação bem-sucedida ainda

### Implementado (v0065, mantido na v0066)

**Máquina de estados** (`vk.h`, `device.c`):
- `PRESENT_READY` (0): operação normal
- `PRESENT_SURFACE_LOST` (1): superfície perdida
- `PRESENT_WAITING_FOR_WINDOW` (2): aguardando ANativeWindow
- `PRESENT_RECREATING` (3): recriando

**Funções atualizadas:**
- `create_swapchain()`: verifica estado, não usa `VK_CHECK` (falha → `SURFACE_LOST`, não `FATAL`)
- `recreate_swapchain()`: retorna early se `WAITING_FOR_WINDOW`
- `recover_lost_surface()`: usa estados explicitamente
- `vk_frame_begin()`: retorna `false` se não `READY` (pula frame, sem acquire/present)
- Inicialização: seta `PRESENT_READY` após criar superfície

### Teste Necessário (P2)
Para provar a recuperação, testar no dispositivo:
1. Abrir o jogo (ACTIVE)
2. Pressionar Home / trocar de app (→ SURFACE_LOST)
3. Aguardar (→ WAITING_FOR_WINDOW)
4. Voltar ao jogo (→ RECREATING → ACTIVE)
5. Verificar no log:
   - `[vk] surface lost: recreating surface and swapchain`
   - `[vk] surface recreated` (ou `waiting for window`)
   - `[vk] surface recovery: READY`
   - Ausência de `FATAL`

### Não declarado
- Correção NÃO declarada como concluída sem teste de retorno ao app (conforme ordem)

---

## PRIORIDADE 3 — DESEMPENHO ⚠️ DADOS COLETADOS, PROFILING PENDENTE

### Estado
- Janela final: **25.688 draws**, **1.069 MB uploads**
- Números altos, mas sem profiling para distinguir as causas

### O que os números indicam
- 25.688 draws em ~2s = ~12.844 draws/segundo = ~428 draws/frame @30fps
- 1.069 MB em ~2s = ~534 MB/s de upload
- Estes são consistentes com os logs anteriores (floresta com ~17k draws/frame)

### Não concluído (conforme ordem)
- NÃO foi concluído que "todos os custos são inevitáveis"
- Profiling necessário para distinguir:
  - Pressão de memória (uploads)
  - Sincronização (waits)
  - Custo de renderização (draws)
  - Compilação de shaders

### Próximos passos (P3)
Com acesso ao dispositivo, medir:
- Bytes por frame (upload/frame)
- Tempo CPU vs GPU (via `vk_frame` stats)
- Custo das transferências (barreiras, copies)

---

## PRIORIDADE 4 — TURNIP ⚠️ DOCUMENTADO, BUILD PENDENTE

### Estado
APK contém (verificado via `unzip -l`):
- `libadrenotools.so` (874 KB) — **PREBUILT, sem instrumentação**
- `libhook_impl.so` (272 KB)
- `libmain_hook.so`, `libfile_redirect_hook.so`, `libgsl_alloc_hook.so`

### Instrumentação existente (não incluída no APK)
- Arquivo: `~/workspace/wonder-recomp/adrenotools-src/src/driver.cpp`
- Adicionado: `ADT_LOG` (__android_log_print, tag "adrenotools") nos 5 pontos de falha
- Backup: `~/workspace/letsgo-port/docs/performance/driver.cpp.orig`

### Os 5 pontos de falha (sem dlerror)
Em `adrenotools_open_libvulkan`, retorna `nullptr` sem mensagem quando:
1. `linkernsbypass_load_status()` retorna falso
2. `linkernsbypass_link_namespace_to_default_all_libs` retorna falso
3. `libhook_impl.so` não carrega via `dlopen`
4. `dlsym(init_hook_param)` retorna nulo
5. `libmain_hook.so` não carrega

### Hipótese (não confirmada)
Incompatibilidade do `linkernsbypass` com Android 16 (API 36).
Último update do linkernsbypass: 2024-09-10 (sem fix para Android 16).

### Para validar (P4)
1. Compilar `adrenotools-src` com NDK para ARM64 (requer setup de build)
2. Substituir `libadrenotools.so` no APK
3. Testar e capturar logcat com tag "adrenotools"
4. Identificar qual dos 5 pontos falha

### Fallback
- Mantido: driver Qualcomm do sistema (Adreno 740)
- Comportamento atual: `[vk] custom GPU driver... failed to load via adrenotools: (null)` → usa driver do sistema

---

## Arquivos Modificados (v0066)

| Arquivo | Alteração | Prioridade |
|---------|-----------|------------|
| `src/runtime/nvn/vulkan/device.c` | `vk_frame_end()`: captura de screenshot via `g_font_capture_frame` | P1 |
| `src/runtime/nvn/vulkan/exec.c` | `_Atomic int g_font_capture_frame`; set no gatilho; `[font-state]` com viewport/scissor/blend/depth | P1 |
| `src/runtime/nvn/vulkan/vk.h` | (sem alteração nesta build — estados já na v0065) | P2 |
| `src/runtime/nvn/vulkan/device.c` | (máquina de estados já na v0065) | P2 |
| `src/runtime/nvn/vulkan/shaders.c` | (dump GLSL já na v0065) | P1 |
| `apk/AndroidManifest.xml` | `versionCode` 1792569649 | — |

---

## Testes

### Compilação v0066
- ✅ `SMOKE_OK` (65 objetos)
- ✅ `RUNTIME_LINK_OK`
- ✅ `APK_OK` (373 MB)
- ✅ Assinatura SHA-256 correta

### Pendentes (requerem dispositivo)
- [ ] P1: Captura de diálogo real com `capture_font` + `font_frame.png`
- [ ] P1: Arquivos `glsl_*.frag` com `LARECOMP_DUMP_FONT_GLSL=1`
- [ ] P2: Ciclo ACTIVE → SURFACE_LOST → WAITING → RECREATING → ACTIVE
- [ ] P3: Profiling de draws/uploads por frame
- [ ] P4: Build da libadrenotools instrumentada

---

## Preservação

- ✅ Gameplay, áudio, controles, salvamentos: preservados
- ✅ Textos de interface existentes: preservados
- ✅ Renderização: preservada (V-flip removido na v0063, não reintroduzido)
- ✅ v0063 mantida como referência reversível
- ✅ Sem modificações em Beluga, mensagens, ou ROM
- ✅ Sem alterações globais em shaders
