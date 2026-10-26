# PERF_AUDIT.md — Auditoria de Desempenho e Gráficos
**Projeto:** Pokémon Let's Go Eevee — port Android (letsgo-recomp)
**Data:** 20/10/2026
**Dispositivo alvo:** Galaxy S23 (Snapdragon 8 Gen 2, Adreno 740, 8 núcleos CPU)
**Sintoma:** 30 fps parado, 20-21 fps andando

---

## 1. Threads

### 1.1 Mapa de threads

| Thread | Criação | Função |
|---|---|---|
| Game threads (várias) | `src/runtime/kernel.c:235` (`pthread_create` em `thread_main`) | Threads do jogo Switch emuladas; o log mostra dezenas de `CreateThread` |
| Render thread (1) | `src/runtime/nvn/vulkan/renderthread.c:129` | Desacopla o game thread da gravação de comandos Vulkan; stack de 64 MB |
| Shader workers (4 no S23) | `src/runtime/nvn/vulkan/exec.c:640` | Compilação GLSL→SPIR-V + criação de pipelines, assíncrona |
| Watch thread | `src/runtime/dispatch.c:291` | Profiling de funções (`fnprof`) |
| Setup thread | `src/runtime/gamesetup.c:243` | Setup inicial |
| Install thread | `src/runtime/mods.c:195` | Instalação de mods |

### 1.2 Contenção

- **Sem afinidade de CPU** (`sched_setaffinity` não é usado em nenhum lugar do runtime). O scheduler do Android decide o posicionamento.
- No S23 (8 núcleos): 4 shader workers + 1 render thread + N game threads competem pelos mesmos 8 núcleos.
- O cálculo de workers (`exec.c:637`): `n = SDL_GetNumLogicalCPUCores() - 4`, clampado em 4..16. No S23: `8 - 4 = 4` workers.
- **Problema:** quando o jogador anda e novos shaders precisam compilar, 4 workers fazem compilação shaderc (CPU-intensiva) concorrendo com as game threads. O shaderc é pesado — cada compilação pode levar 10-50 ms.
- A render thread tem `MAX_PRESENTS = 2` (`renderthread.c:55`): o jogo pode rodar até 2 frames à frente do que é exibido. Isso esconde latência mas não aumenta throughput.

### 1.3 Recomendação

- **P1:** Reduzir workers de shader para 2-3 no S23, ou usar afinidade para isolá-los nos núcleos efficiency. A compilação de shaders é importante mas não deve roubar CPU das game threads durante gameplay.
- **P2:** O pipeline de shaders tem cache em disco (`cache/spirv/`), então após a primeira visita a uma área, as compilações diminuem. O problema é o "first-visit stutter".

---

## 2. Shaders

### 2.1 Pipeline

```
NVN program → GLSL (tradução) → shaderc → SPIR-V → vkCreateShaderModule → vkCreate*Pipelines
```

- **Tradução:** `src/runtime/nvn/vulkan/exec.c` (`translate_program`, chamado via `run_job`)
- **Compilação:** `src/runtime/nvn/vulkan/shaders.c:134` (`vk_glsl_to_spirv`)
  - Usa `shaderc` com `shaderc_optimization_level_zero` por padrão (linha 141-142)
  - **Cache em disco:** `<data_root>/cache/spirv/<FNV-1a hash>.spv` (linhas 24-30)
  - **Pós-processamento:** `no_contraction()` adiciona `NoContraction` a operações float em vertex shaders (linhas 77-118) — necessário para correção (depth pre-pass vs colour pass)
- **Assíncrono:** Sim. `run_job()` (exec.c:634) enfileira para workers. Draws que precisam de programas não prontos são **pulados** (não travam), exceto quando `wait_built()` é chamado (exec.c:2348).

### 2.2 Estatísticas

O log `[vk]` a cada 2s reporta (`exec.c:767-772`):
```
%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes,
wait %.0fms, compile %d (translate %.0fms, spirv %.0fms, pipelines %.0fms, draws waited %.0fms),
fbcopy %llu, upload %.1fMB
```

- `skipped`: draws pulados por programa não pronto (com breakdown do motivo)
- `compile`: número de compilações no intervalo, com tempo em translate/spirv/pipelines
- `draws waited`: tempo que draws esperaram por programas (via `wait_built`)

### 2.3 Stutter

- **First-visit:** Ao entrar numa área nova, dezenas de pipelines compilam de uma vez. Os workers fazem isso em background, mas os draws são pulados até os programas ficarem prontos → objetos "pop-in" ou frames com menos draws.
- O cache em disco mitiga nas visitas subsequentes, **mas o APK não vem com shaders pré-compilados** (`apk/assets/spirv/` está vazio; `shaders.c` loga `[shader] no bundled SPIR-V in APK`).
- **Recomendação P1:** Pré-compilar os shaders jogando e embutir o `cache/spirv/` no APK (como o Wonder faz com 12.757 shaders). Isso elimina o stutter de first-visit.

---

## 3. Texturas

### 3.1 Upload e detecção de mudanças

`src/runtime/nvn/vulkan/textures.c:471` (`upload_texture`):

1. **Texturas dinâmicas** (flag `0x10`): marcadas via `b->dynamic = true` (linha 506). São **hasheadas por completo a cada frame** que são usadas.
2. **Texturas estáticas:** hash amostral (`fnv_sample`, 4096 amostras) por padrão; hash completo a cada 120 frames, com **orçamento de 4 MB/frame** (linhas 507, 512).
3. **Hash:** `hash_full()` (linhas 427-444) — 4 lanes independentes, ~1.5 bytes/ciclo por lane serial. Uma textura 1024x1024 RGBA (4 MB) custa ~2.7M ciclos por hash (~0.9 ms num núcleo 3 GHz).
4. **Deswizzle:** `tegra_deswizzle()` (linha 561) — CPU, para texturas tiled. É um shuffle de memória memory-bound.

### 3.2 Gargalo

- Quando o jogador **anda**, novas texturas do mundo fazem streaming. Cada textura nova precisa: hash completo + deswizzle (se tiled) + upload via staging buffer.
- O `stat_upload_bytes` no log `[vk]` mostra MB/frame enviados à GPU.
- **O hash de texturas dinâmicas a cada frame é caro.** Se o jogo marca muitas texturas como dinâmicas (flag 0x10), o custo de CPU é significativo.

### 3.3 ASTC

- `textures.c:274-277`: Se a GPU não suporta ASTC, decodifica na CPU via `astc_decode_ldr()` (muito caro).
- **Adreno 740 suporta ASTC nativamente** → este caminho NÃO é usado no S23. OK.

### 3.4 Recomendação

- **P2:** Verificar no log `[vk]` o `upload X.XMB` durante caminhada. Se > 10 MB/frame, o streaming de texturas é um gargalo.
- **P2:** O orçamento de 4 MB/frame para re-hash de texturas estáticas pode ser baixo quando muitas texturas mudam (ex: atlas de glifos). Considerar aumentar para 8 MB.

---

## 4. Resolução e Escala

### 4.1 Configuração atual

- `src/runtime/platform/android.c:67`: `setenv("LARECOMP_SCALE", "0.6667", 1);`
- `src/runtime/nvn/output_scale.c:74`: `if (forced > 0) h = forced;` — **tem precedência total**.
- **Resolução interna fixa:** 1920×0.6667 × 1080×0.6667 = **1280×720**.
- **Resolução dinâmica DESABILITADA:** O `RES_DYNAMIC` (`output_scale.c:80-87`) nunca é alcançado porque `forced > 0` sempre vence. O jogo **não pode reduzir a resolução** quando a GPU não acompanha.

### 4.2 Upscaler

- Padrão: **FSR1** (`settings.h:8`: `UPSCALE_SHARP = UPSCALE_FSR1`).
- `exec.c:3322`: FSR1 só roda quando há upscale (`ow > b->width + 1`).
- Com 720p interno → ~1080p saída, o FSR1 está ativo.

### 4.3 SMAA — ACHADO CRÍTICO

- **SMAA está LIGADO por padrão** (`src/runtime/settings.c:31`: `.antialias = AA_SMAA`).
- `exec.c:3318-3337`: Quando SMAA está ligado, são **3 passes fullscreen em 720p** por frame:
  1. `smaa_edge.frag` (detecção de bordas por luma)
  2. `smaa_weight.frag` (cálculo de pesos de blending, 3 texturas)
  3. `smaa_blend.frag` (blending de vizinhança)
- **Cadeia completa de pós-processamento por frame:**
  1. SMAA edge (1280×720)
  2. SMAA weight (1280×720)
  3. SMAA blend (1280×720)
  4. FSR1 EASU (1280×720 → ~1920×1080, 12 taps com ALU pesada)
  5. FSR1 RCAS (1920×1080, 5 taps)
  6. Present (fullscreen triangle)

**São 6 passes fullscreen por frame!** No Adreno 740, isso é uma carga significativa de GPU, especialmente o EASU do FSR1 que tem ALU pesada por pixel.

### 4.4 Recomendação

- **P0 (maior impacto):** Desligar SMAA por padrão no Android, ou trocar por uma alternativa mais barata. Os 3 passes SMAA em 720p + 2 passes FSR1 em 1080p provavelmente consomem 30-40% do tempo de GPU do frame.
  - O launcher já tem checkbox "Suavizar serrilhado (SMAA)", mas vem marcado por padrão.
  - Teste A/B: medir fps com `smaa = 0` no `larecomp.ini`.
- **P1:** Reabilitar a resolução dinâmica no Android (remover ou condicionar o `LARECOMP_SCALE` forçado). Com dynres, o jogo reduziria de 720p para ~540p automaticamente ao andar, mantendo 30 fps.

---

## 5. Draw Calls

### 5.1 Contagem

- O log `[vk]` reporta `draws` e `skipped` a cada 2 segundos (`exec.c:767`).
- `stat_draws`: draws submetidos; `stat_skipped`: draws pulados (programa não pronto).
- Sem um log dela, não temos o número exato para Let's Go. Jogos Pokémon Switch tipicamente têm 500-2000 draws por frame no overworld.

### 5.2 Vertex data

- `exec.c:776`: `vertex ranges %llu copied, %llu in place`.
- `exec.c:2095`: Se `ring_gpu_enabled()` e não há overlap, os dados de vértice são **copiados** para VRAM; senão, lidos **in place** (via host-visible memory).
- Cópias de vértices a cada frame consomem banda de memória.

### 5.3 O padrão 30→20 fps

Andar aumenta:
1. **Draws:** novos objetos/chunks do mundo entram no frustum.
2. **Uploads de textura:** streaming de texturas do mundo (`upload X.XMB` no log).
3. **Compilações de shader:** novos materiais → workers de shader competem com game threads.
4. **Hash de texturas:** mais texturas dinâmicas para hashear por frame.

### 5.4 Recomendação

- **P1:** Pedir o log `[vk]` dela durante caminhada para ver: draws, upload MB, compile count, draws waited. Isso identifica qual dos 4 fatores domina.
- **P2:** Se `skipped` for alto durante caminhada, é stutter de shader — mitigado pelo cache pré-compilado (P1 da seção 2).

---

## 6. Gargalos Específicos do Adreno 740

### 6.1 Pós-processamento excessivo

Como detalhado na seção 4.3, os 6 passes fullscreen são o maior custo fixo de GPU. O Adreno 740 é potente, mas:
- FSR1 EASU tem ~40 operações ALU por pixel + 12 taps de textura.
- Em 1920×1080 = 2M pixels × 40 ALU = 80M ALU ops só no EASU.
- SMAA weight tem múltiplos lookups nas LUTs.

### 6.2 Tile-based rendering

O Adreno 740 é um GPU **tile-based deferred renderer (TBDR)**. Características:
- **Passes fullscreen são caros** porque cada pass precisa escrever o tile na memória principal (resolve) e o próximo pass precisa ler de volta. Os 6 passes = 6 resolves de tiles.
- **MSAA seria caro**, mas o jogo não usa MSAA (usa SMAA pós-processado).
- **Recomendação:** Reduzir o número de passes (desligar SMAA) tem impacto desproporcionalmente positivo em TBDR.

### 6.3 Driver

- **Adrenotools está PAUSADO** (`device.c:29-32`: `#ifdef LETSGO_ADRENOTOOLS` desativado).
- O driver do sistema (Adreno 740 proprietary) está em uso.
- O `turnip-8g2.zip` (Mesa Turnip) está em `apk/assets/` mas **inativo**.
- O Wonder roda a ~42 fps com Turnip no mesmo S23. O driver proprietário Adreno pode ter overhead maior no padrão de uso deste runtime (muitos pipelines, descriptors).
- **Recomendação P1:** Ativar o adrenotools/Turnip (trabalho em andamento pelo parent agent). O Turnip tem melhor desempenho em workloads com muitos draws pequenos.

### 6.4 Present mode

- `device.c:498-499`: **MAILBOX** por padrão. Com o jogo a 30 fps e vsync a 60 Hz, MAILBOX descarta frames extras (bom para latência).
- O swapchain é 1080×2340 (portrait) com `preTransform = ROTATE_90`. O present faz rotação via UV no shader (`present.vert`), não via hardware — custo mínimo (1 pass).

---

## 7. Resumo de Recomendações Priorizadas

| Prioridade | Ação | Arquivo:Linha | Impacto esperado |
|---|---|---|---|
| **P0** | Desligar SMAA por padrão no Android (3 passes fullscreen economizados) | `src/runtime/settings.c:31` | Alto (GPU) |
| **P0** | Medir com `smaa=0`: pedir log `[vk]` dela andando | — | Diagnóstico |
| **P1** | Pré-compilar shaders e embutir `cache/spirv/` no APK | `apk/assets/spirv/` (vazio hoje) | Elimina stutter |
| **P1** | Reabilitar resolução dinâmica no Android | `src/runtime/platform/android.c:67`, `src/runtime/nvn/output_scale.c:74` | 30 fps estáveis |
| **P1** | Ativar adrenotools/Turnip | `src/runtime/nvn/vulkan/device.c:29` | +40% fps (ref. Wonder) |
| **P2** | Reduzir shader workers de 4 para 2-3 no S23 | `src/runtime/nvn/vulkan/exec.c:637` | Menos contenção CPU |
| **P2** | Aumentar orçamento de re-hash para 8 MB/frame | `src/runtime/nvn/vulkan/textures.c:507` | Menos re-uploads |
| **P3** | Considerar FSR1 → bilinear (economiza 2 passes) | `src/runtime/settings.h:8` | Médio (GPU) |

---

## 8. Notas

- **FPS counter e gamepad overlay NÃO estão no caminho de renderização.** O FPS é um `TextView` Android atualizado via JNI a cada 500 ms; o overlay são Views compostas pelo SurfaceFlinger. Impacto desprezível.
- **O diagnóstico `[tex-audit]`** (adicionado para a investigação de texto) faz um scan linear completo de texturas R8/RG8 a cada `upload_texture`. É temporário e deve ser removido após a investigação de texto, pois adiciona overhead de CPU proporcional ao tamanho das texturas.
- **O log `[vk]` a cada 2s** é a ferramenta principal para diagnóstico contínuo. Pedir a ela: jogar 30s andando e enviar o log. As linhas `draws`, `upload X.XMB`, `compile N (...)`, `draws waited` identificam o gargalo dominante.
