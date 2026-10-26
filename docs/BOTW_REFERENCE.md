# BOTWRecompAndroid — Referência Técnica para o Let's Go

**Repositório:** https://github.com/mamcjr/BOTWRecompAndroid
**Clonado em:** `~/workspace/botw-recomp-android-ref/` (2026-10-25)
**Base:** LASRecomp (peiikai) + LASRecompAndroid (mamcjr) — mesma família do port Let's Go
**Status:** Jogável no POCO F7 Ultra (Adreno 830, Turnip), 30-60 fps

## Achado crítico: texto invisível (BOTW teve o MESMO bug)

**Documento:** `docs/BOTW-PORT.md` linha 197

> **Nenhum texto (menus, títulos)** | os glifos de cada linha de texto ficam num buffer ~2,7 GB dentro do heap de 3 GB; o descritor do heap alcança só `maxStorageBufferRange` (2 GB no Adreno): o vertex shader lia zeros | com o heap na GPU (Turnip) e maior que esse alcance, as draws usam a tabela GMap numa janela do heap que começa nos storage buffers delas (`gwin_prepare`, como o compute já fazia); `LARECOMP_VK_NO_GWIN=1` desliga

### Mecanismo do bug

1. O vertex shader do texto lê os glifos de um buffer na memória global (gmem/heap)
2. Se esse buffer está além de `maxStorageBufferRange` (2 GB no Adreno 740), o descritor Vulkan é clampado
3. Leituras além do alcance retornam **zeros**
4. Zeros → todos os vértices em (0,0,0) → quads degenerados → **nada rasterizado**
5. O fragment shader nunca executa → **nenhum texto, nenhum magenta diagnóstico**

### Por que isso explica o Let's Go

- **v0075 provou:** magenta FS compilado + pipeline reconstruído, mas **nada aparece**
- **VS do texto no Let's Go lê gmem:** `glsl_233e04b37cb9bc91.vert` (25 refs `gmem_`), `glsl_c19da60baaa251eb.vert` (14 refs `gmem_`)
- **Let's Go NÃO tem o gwin:** `grep gwin ~/workspace/letsgo-port/src/runtime/nvn/vulkan/exec.c` = vazio
- **Let's Go retorna o heap inteiro:** `gmem_info()` retorna `{gmem_buf, 0, gmem_size}` sem clamp — se `gmem_size > 2 GB`, o descritor é inválido/clampado pelo driver

### O fix (portar do BOTW)

**Arquivo:** `runtime/nvn/vulkan/exec.c` no BOTW (linhas 3455, 3972-4010, 5300)

1. **`gwin_mode`:** ativado quando `heap_size > maxStorageBufferRange` e sem `LARECOMP_VK_NO_GWIN`
2. **`gwin_prepare(stage_mask)`:** antes de cada draw, calcula uma janela do heap que começa no primeiro storage buffer do draw e cobre todos os buffers usados, dentro do alcance do descritor
3. **GMap table:** o shader traduzido usa a tabela GMap para mapear endereços globais → offsets dentro da janela
4. **`uses_gmem_map`:** flag no `translate.c` (linha 1777) marca shaders que usam o mapa

**Arquivos BOTW para portar:**
- `runtime/nvn/vulkan/exec.c`: `gwin_prepare()`, `gwin_mode`, `gmap_tab`, `GMAP_MAX`
- `runtime/nvn/maxwell/translate.c`: `uses_gmem_map`, geração da GMap no GLSL
- `runtime/nvn/vulkan/shaders.c`: binding da GMap

## Outras técnicas úteis

### Driver compatibility (Nycolazs / LASRecomp-Android-Plus)
- `runtime/nvn/vulkan/device_compat.h`: workarounds por driver
- `runtime/nvn/vulkan/android_driver.c`: carregamento de driver via adrenotools
- `docs/ANALISE-DRIVER-PADRAO-2026-10-21.md`: análise do driver padrão

### Performance
- `docs/DESEMPENHO-60FPS-2026-10-23.md`: técnicas para 60 fps
- `docs/MODS-DESEMPENHO-BOTW.md`: mods de desempenho
- FSR 1 / Snapdragon GSR upscaling, SMAA

### Texturas
- Ciclo de vida de texturas (deduplicação, liberação com delay de 4 quadros)
- BC decodificado na CPU para drivers sem memória "placed"

## Limites honestos

- BOTW foi testado só no POCO F7 Ultra (Adreno 830) — o S23 tem Adreno 740, mesmo limite de 2 GB
- O fix gwin é específico para heaps > `maxStorageBufferRange` — precisa confirmar o heap size do Let's Go no S23
- Se o heap do Let's Go for < 2 GB, o bug é outro (mas os sintomas são idênticos)
