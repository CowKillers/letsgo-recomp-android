# AUDITORIA GRÁFICA COMPLETA — Let's Go Android Port

**Data:** 2026-10-20
**Alvo:** Galaxy S23 (Snapdragon 8 Gen 2, Adreno 740, 8GB RAM)
**Código auditado:** `~/workspace/letsgo-port/src/runtime/`
**Sintomas:** Floresta 13-15 fps (17k-19k draws, 576-628MB transient/2s, GPU wait 2.2-2.6s) | Externas 19-21 fps | Internas 27-30 fps

---

## P0 — CRÍTICO (causam os 13 fps na floresta)

### P0-1: 17k-19k draw calls por frame — overhead insustentável no Adreno 740
**Arquivo:** `src/runtime/nvn/vulkan/exec.c` (sistema de draws)
**Evidência:** Log mostra `17707 draws, 19073 draws` na floresta vs ~12k em áreas normais.
**Impacto:** Cada draw tem custo fixo de CPU (setup de estado, binding de descritores, validação) + custo de GPU (troca de contexto). 17k draws é 34x o típico de um jogo mobile (100-500 draws). O Adreno 740 não consegue processar 17k draws a 30fps.
**Causa raiz:** Conteúdo do jogo (vegetação da floresta = milhares de meshes individuais). O runtime traduz corretamente, mas não há merging de draws.
**Fix sugerido:** Não há fix no runtime sem mudar o jogo. Mitigações: (1) Turnip tem menor overhead de draw call que o driver Adreno; (2) reduzir resolução ajuda fill-rate mas NÃO reduz overhead de draw call.

### P0-2: GPU wait 2.2-2.6 SEGUNDOS — GPU severamente gargalada
**Arquivo:** `src/runtime/nvn/vulkan/exec.c:76-82` (`vk_wait_seq`)
```c
uint64_t t0 = SDL_GetTicksNS();
VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
stat_wait_ns += SDL_GetTicksNS() - t0;
```
**Evidência:** Log: `wait 2249ms, wait 2652ms` em intervalo de 2s. O wait EXCEDE o intervalo — a CPU fica bloqueada esperando a GPU.
**Impacto:** 2.2s de stall por 2s de jogo = GPU a 110% de utilização. Impossível atingir 30fps.
**Causa raiz:** Combinação de 17k draws + texturas grandes + shaders complexos da vegetação.
**Fix sugerido:** Reduzir draws (ver P0-1) ou usar Turnip (ver P1-5).

### P0-3: Flag `dynamic` de textura é catraca de mão única — nunca reseta
**Arquivo:** `src/runtime/nvn/vulkan/textures.c:636`
```c
if (b->uploaded && h != b->data_hash) b->dynamic = true;
```
**Problema:** Uma vez que `b->dynamic = true`, NUNCA volta para `false`. Qualquer textura que mude UMA vez (ex: render target, textura de streaming, atlas de fonte) será marcada como dinâmica PARA SEMPRE.
**Impacto:** Texturas dinâmicas fazem hash FULL (leitura completa da memória) TODO frame (linha 625: `bool full = b->dynamic || ...`). Se 100 texturas forem marcadas dinâmicas, são 100 leituras completas por frame, mesmo que nunca mais mudem.
**Evidência:** Linha 619: `if (t->flags & 0x10) b->dynamic = true;` — flag 0x10 do NVN também marca como dinâmica permanentemente.
**Fix sugerido:** Resetar `b->dynamic = false` se o hash ficar estável por N frames (ex: 120 frames sem mudança). Ou usar contador de frames desde última mudança.

---

## P1 — ALTO (impacto significativo)

### P1-1: Turnip não carrega — perde ~2x de performance
**Arquivo:** `src/runtime/nvn/vulkan/device.c:58-68`
**Evidência:** Log: `[vk] custom GPU driver .../turnip-8g2/libvulkan_freedreno.so failed to load via adrenotools: (null)`
**Impacto:** No Wonder, Turnip dá ~2x fps (42 fps vs ~20 fps no driver sistema). No Let's Go, o driver sistema é usado sempre.
**Causa raiz:** `adrenotools_open_libvulkan()` retorna NULL sem setar `dlerror()` (retorna "(null)"). A falha é DENTRO do adrenotools, não no dlopen. Possíveis causas: (1) hook libs não encontradas no `LARECOMP_NATIVE_LIB_DIR`; (2) incompatibilidade de versão; (3) falha silenciosa de inicialização.
**Fix sugerido:** Adicionar log detalhado dentro do adrenotools (não temos o fonte) OU comparar byte-a-byte a inicialização com o Wonder (que funciona). Verificar se `libhook_impl.so` está no `nativeLibraryDir` no aparelho.

### P1-2: Compilação de shaders em runtime bloqueia draws
**Arquivo:** `src/runtime/nvn/vulkan/exec.c:2344-2362`
**Evidência:** Log: `compile 195 (translate 453ms, spirv 756ms, pipelines 143ms, draws waited 149ms)` — em UM intervalo de 2s, 195 shaders compilados, custando 1.2s de CPU!
**Impacto:** `draws waited 149ms` — draws ficam bloqueados esperando pipelines. Na entrada de área nova (floresta), centenas de shaders compilam de uma vez, causando stutter.
**Causa raiz:** APK não shippa cache SPIR-V pré-compilado (pasta `assets/spirv` vazia). Cada shader é traduzido (Maxwell→GLSL) + compilado (GLSL→SPIR-V via shaderc) na hora.
**Fix sugerido:** (1) Extrair o cache SPIR-V do aparelho (`$LARECOMP_DATA_ROOT/cache/spirv/`) e embutir no APK como o Wonder faz (12,757 shaders); (2) reduzir workers de 4 para 2-3 para diminuir contenção de memória.

### P1-3: 576MB de transient por 2s (19MB/frame) — alto mas é sintoma
**Arquivo:** `src/runtime/nvn/vulkan/exec.c:480,494` (`transient_from`)
```c
stat_upload_bytes += size;  // conta TODA alocação transient, não só texturas
```
**Evidência:** Log: `upload 576.1MB, upload 628.9MB` por intervalo de 2s = 19-21MB por frame.
**Impacto:** O "upload" do log NÃO é bytes enviados à GPU — é bytes ALOCADOS no pool transient (vertex data + uniforms + staging de texturas). Com 17k draws, cada um aloca ~1.1KB em média = 19MB/frame. É alto, mas é consequência dos 17k draws (P0-1), não causa raiz.
**Atenuação existente:** `guest_copy_pool` (4096 slots) dedupa cópias de memória guest por submission; `copy_dedupe` dedupa uniforms repetidos. Mas 17k draws > 4096 slots = thrashing no cache de dedup.
**Fix sugerido:** Aumentar `GCOPY_TAB` de 4096 para 16384. Custo: +128KB de RAM. Benefício: menos alocações duplicadas.

### P1-4: SMAA ligado por padrão — 6 passes fullscreen desperdiçados
**Arquivo:** `src/runtime/settings.c:31`
```c
.antialias = AA_SMAA,  // (the game has no anti-aliasing of its own)
```
**Impacto:** SMAA 1x = 6 passes fullscreen por frame (edge detection, blending, etc.). Em 1080p, são 6x 8MB = 48MB de bandwidth por frame só para AA.
**Status:** A Emily já desligou no ini dela (`smaa = 0`). Mas o DEFAULT é ligado — qualquer usuário novo sofre sem saber.
**Fix sugerido:** Mudar default para `AA_OFF` no Android (telas pequenas não precisam de SMAA, e o custo é alto no Adreno).

---

## P2 — MÉDIO (otimizações)

### P2-1: 4 shader workers — muita memória para 8GB RAM
**Arquivo:** `src/runtime/nvn/vulkan/shaders.c:135` ("compiler per thread")
**Impacto:** Cada worker tem um compilador shaderc + contexto. 4 workers + render thread + game threads = 6+ threads competindo por CPU e RAM. O S23 tem 8 cores, mas 8GB RAM é apertado com o jogo + runtime + shaders.
**Fix sugerido:** Reduzir para 2-3 workers via variável de ambiente ou `#ifdef __ANDROID__`.

### P2-2: Budget de re-hash de texturas estáticas (4MB/frame)
**Arquivo:** `src/runtime/nvn/vulkan/textures.c:621`
```c
if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
```
**Impacto:** Texturas estáticas fazem hash completo a cada 120 frames, limitado a 4MB/frame. Com 100MB de texturas, leva 25 frames (0.8s) para re-verificar tudo. É razoável, não é gargalo.
**Status:** OK, não precisa mudar.

### P2-3: FSR1 upscaler — custo do upscale
**Arquivo:** `src/runtime/nvn/vulkan/exec.c:3322`
```c
bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);
```
**Impacto:** FSR1 roda EASU (upscale) + RCAS (sharpening) = 2 passes fullscreen. Em 540p→1080p, o custo é moderado (~2-3ms no Adreno 740). É necessário para a resolução escalável funcionar bem.
**Status:** OK, o custo é justificado pelo ganho de performance da resolução menor.

### P2-4: Texturas pequenas de fonte (51x R8 zeradas)
**Arquivo:** `src/runtime/nvn/vulkan/textures.c` (tex-audit)
**Evidência:** 51 texturas R8 (64x64 até 4x4) em `0x7285e32000`, todas com `nonzero=0`.
**Impacto:** Nenhum impacto em performance — são 51 x 4KB = 204KB de VRAM desperdiçada. O problema é funcional (texto do diálogo vazio), não performance.
**Status:** Investigação pausada a pedido da usuária. Não afeta fps.

---

## Resumo por impacto no fps

| # | Problema | Arquivo:Linha | Severidade | Ganho estimado se corrigido |
|---|----------|---------------|------------|----------------------------|
| 1 | 17k draws na floresta | exec.c (sistema) | P0 | +10-15 fps (13→25 fps) |
| 2 | GPU wait 2.2s | exec.c:76-82 | P0 | Resolve junto com #1 |
| 3 | Flag dynamic sem reset | textures.c:636 | P0 | +2-5 fps (reduz hashing) |
| 4 | Turnip não carrega | device.c:58-68 | P1 | +50-100% fps (2x no Wonder) |
| 5 | Shaders compilam em runtime | exec.c:2344 | P1 | Elimina stutter na entrada de áreas |
| 6 | Transient 19MB/frame | exec.c:480,494 | P1 | +1-2 fps (menos pressão de memória) |
| 7 | SMAA default ON | settings.c:31 | P1 | +3-5 fps para novos usuários |
| 8 | 4 shader workers | shaders.c:135 | P2 | +1 fps, menos RAM |

---

## Recomendações prioritárias (ordem de implementação)

1. **Turnip (P1-4):** Maior ganho potencial (2x). Investigar por que `adrenotools_open_libvulkan` retorna NULL. Comparar inicialização com o Wonder byte-a-byte.

2. **Dynamic flag reset (P0-3):** Fix simples, ganho garantido. Adicionar contador: se hash estável por 120 frames, `b->dynamic = false`.

3. **Cache SPIR-V pré-compilado (P1-2):** Extrair do aparelho e embutir no APK. Elimina stutter de compilação.

4. **SMAA default OFF (P1-4):** Uma linha, ajuda novos usuários.

5. **Aumentar GCOPY_TAB (P1-3):** De 4096 para 16384. Reduz alocações duplicadas com 17k draws.

6. **Reduzir shader workers (P2-1):** De 4 para 2 no Android. Menos contenção.

**Nota sobre 17k draws (P0-1):** Não há fix no runtime — é conteúdo do jogo. As mitigações são Turnip (menor overhead) e aceitar que áreas densas serão mais lentas.
