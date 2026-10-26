# 00_CODE_AUDIT.md

## Auditoria de Código — Runtime Vulkan Let's Go Android
**Data:** 2026-10-21
**Missão:** Performance 30 FPS no Snapdragon 8 Gen 2

---

## 1. Inicialização Vulkan

**Arquivo:** `src/runtime/nvn/vulkan/device.c`

**Funções principais:**
- `load_gipa()` (linha 46): Resolve o loader Vulkan
  - Tenta `adrenotools_open_libvulkan()` se `LARECOMP_GPU_DRIVER` configurado
  - Fallback para loader do sistema
  - **Ponto crítico:** `adrenotools` retorna NULL sem `dlerror()` (falha silenciosa)
- `vk_init()` (não verificado linha exata): Cria instância, dispositivo, filas
- `vkGetPhysicalDeviceProperties()` (linha 492): Obtém `Adreno (TM) 740`
  - **Adicionado em v0054:** Log `[vk] active GPU:` para diagnóstico

**Dependências:**
- `vk_dispatch.h`: Dispatch de 127 PFNs (global→instance→device)
- `adrenotools/driver.h`: Loader de driver customizado
- SDL3: `SDL_Vulkan_CreateSurface()` → substituído por `vkCreateAndroidSurfaceKHR` direto

**Pontos críticos:**
- `System.loadLibrary()` usa `RTLD_LOCAL` → símbolos do adrenotools não exportados para `liblasgame.so`
  - **Fix aplicado:** Link com `-ladrenotools` (DT_NEEDED) na v24-adreno3 do Zelda; verificado no Let's Go?
- `ANativeWindow` obtido via JNI de `SDLActivity.mSurface`
- `preTransform` IDENTITY **NUNCA** forçar (congela Adreno 740 - incidente v1030)

---

## 2. Loader Turnip / Adrenotools

**Arquivo:** `src/runtime/nvn/vulkan/device.c` linhas 46-80

**Fluxo:**
1. Java seta `LARECOMP_GPU_DRIVER=<dir>/<lib>.so` e `LARECOMP_NATIVE_LIB_DIR`
2. `load_gipa()` verifica `access(drv, R_OK)` (padrão Wonder, adicionado v0052)
3. Chama `adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, NULL, libdir, dir, libname, NULL, NULL)`
4. Se NULL → log `failed to load via adrenotools: (null)` → usa driver do sistema

**Estado atual:**
- Arquivo é legível (`access()` passa)
- `adrenotools` retorna NULL, `dlerror()` = `(null)`
- **Causa raiz desconhecida** — falha interna do adrenotools
- Driver ativo confirmado: `Adreno (TM) 740` (sistema), Turnip nunca carregou

**Arquivos Java:**
- `apk/java/com/letsgorecomp/GpuDrivers.java`: Gerencia drivers em `<filesDir>/gpu_drivers/<slug>/`
- `LetsGoActivity.java`: Seta env vars antes de `System.loadLibrary("lasrecomp")`

---

## 3. Tradução NVN → Vulkan

**Arquivos:**
- `src/runtime/nvn/maxwell/translate.c`: Maxwell → GLSL
- `src/runtime/nvn/vulkan/shaders.c`: GLSL → SPIR-V via shaderc

**Pipeline:**
1. Jogo submete comandos NVN (Maxwell)
2. `translate.c` converte para GLSL
3. `shaders.c:vk_glsl_to_spirv()` compila via shaderc
4. Cache em `$LARECOMP_DATA_ROOT/cache/spirv/%016llx.spv`
5. Chave: FNV-1a do GLSL + stage + versão (`+3`)

**Ponto crítico — `cbr` macro (translate.c:1747):**
```glsl
#define cbr(cb, off) cb[min((off) >> 4, uint(cb.length() - 1))][((off) >> 2) & 3u]
```
- Usado para loads indexados da constant buffer ("corner-indexed")
- O `min()` pode mascarar índice fora dos limites
- **Suspeita:** Bug do texto invisível está aqui (pista do Wonder)

**Workers:**
- `exec.c`: 3 workers no Android (v0058, padrão LASRecomp-Android-Plus)
- Compilação assíncrona; draws esperam pipelines (`wait_built`)

---

## 4. Gerenciamento de Texturas

**Arquivo:** `src/runtime/nvn/vulkan/textures.c`

**Estruturas:**
- `NvnTexture`: Metadados (width, height, format, addr guest)
- `TexBackend`: Backend Vulkan (`VkImage`, `VkImageView`, `data_hash`, `dynamic`, `dyn_stable`)

**Upload:**
- `upload_texture()`: CPU → GPU via staging buffer
- **Hash amostral:** `fnv_sample()` para buffers > 4096 bytes (otimização)
- **Full hash:** A cada 120 frames ou se `dynamic`
- **Fix v0053:** `dyn_stable` contador — reseta `dynamic=false` após 120 frames estáveis

**Formatos:**
- `fmt_of(0x01)` → `VK_FORMAT_R8_UNORM` (fontes Beluga)
- Atlas 2048x1024 R8 em `0x729c1bc000` (tem glifos!)
- 51 texturas 64x64 R8 em `0x7285e32000` (vazias)

**Cache:**
- `GCOPY_TAB` 16384 entradas (v0053, era 4096) — dedup de cópias guest

---

## 5. Render Passes e Sincronização

**Arquivo:** `src/runtime/nvn/vulkan/exec.c`

**Estatísticas (log 170):**
- Draws: até 22.521 por janela de 2s
- Passes: até 2.016
- Flushes: 74-84
- Wait: até 2.930 ms

**Sincronização:**
- `vk_wait_seq()` (linhas 76-82): CPU bloqueia esperando GPU
- Fences e semáforos (timeline?)
- **NÃO** usa o watcher thread do LASRecomp-Android-Plus (otimização não portada)

**Command buffers:**
- Ring buffer por frame in flight
- `vkQueueSubmit` + `vkQueuePresentKHR`

---

## 6. Swapchain e Superfície Android

**Arquivos:** `device.c`, `src/runtime/platform/android.c`

**Fluxo:**
1. `nativeSetup()` (Java) → captura `JavaVM*`, `ANativeWindow*` de `SDLActivity.mSurface`
2. `vkCreateAndroidSurfaceKHR()` com `ANativeWindow`
3. Swapchain com `preTransform` do driver (NUNCA forçar IDENTITY)

**Ciclo de vida:**
- `VK_ERROR_SURFACE_LOST_KHR` no log → superfície perdida ao minimizar
- **Pendente:** Implementar recuperação robusta (ETAPA 9)

---

## 7. FSR1 e Resolução

**Arquivos:**
- `src/runtime/nvn/output_scale.c`: `LARECOMP_SCALE` env var
- `apk/java/com/letsgorecomp/LetsGoActivity.java`: Lê SharedPreferences

**Mapeamento (v0051):**
- 0.5x (480p) → `0.4444`
- 0.75x (540p) → `0.5`
- 1x (720p) → `0.6667`
- 1.5x (1080p) → `1.0`

**FSR1:** Aplicado após renderização em 1280x720 → upscale para 1920x1080

---

## 8. Gerenciamento de Memória

**Arquivo:** `src/runtime/nvn/vulkan/vma.cpp` (Vulkan Memory Allocator)

**Uploads (log 170):**
- Até 883 MB por janela de 2s
- **Interpretação:** Soma de alocações transient, não bytes finais
- ~19-21 MB/frame em janelas de 2s
- **Causa:** 22k draws → muitos uploads pequenos

---

## 9. Pontos Críticos Resumidos

| # | Componente | Problema | Impacto |
|---|------------|----------|---------|
| 1 | Turnip loader | `adrenotools` retorna NULL sem erro | Sem ganho de performance do Turnip |
| 2 | Draw calls | 22k/frame na floresta (conteúdo do jogo) | 13-18 FPS, não corrigível no runtime |
| 3 | Uploads | 883 MB/janela (sintoma dos draws) | Wait 2.9s |
| 4 | Texto | Atlas tem glifos, binding OK, mas invisível | Bug no `cbr`/UV do tradutor |
| 5 | Surface | `VK_ERROR_SURFACE_LOST_KHR` ao minimizar | Crash/falha na recuperação |
| 6 | Sync | Sem watcher thread (otimização não portada) | CPU bloqueia em `vk_wait_seq` |

---

## 10. Arquivos por Responsabilidade

| Arquivo | Responsabilidade |
|---------|------------------|
| `device.c` | Init Vulkan, loader, surface, swapchain |
| `exec.c` | Draws, passes, sync, workers, FSR |
| `textures.c` | Upload, cache, formatos |
| `shaders.c` | GLSL→SPIR-V, pipeline cache |
| `translate.c` | Maxwell→GLSL |
| `vma.cpp` | Alocação de memória GPU |
| `vk_dispatch.h` | Dispatch de funções Vulkan |
| `android.c` | JNI, `ANativeWindow`, ciclo de vida |
| `LetsGoActivity.java` | Env vars, GPU driver, resolução |
| `GpuDrivers.java` | Gerenciamento de drivers Turnip |
