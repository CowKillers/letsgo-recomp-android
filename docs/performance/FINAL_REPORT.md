# FINAL_REPORT.md

## Relatório Final — Missão Performance Let's Go Android
**Data:** 2026-10-21
**Objetivo:** 30 FPS estáveis no Snapdragon 8 Gen 2

---

## Resumo Executivo

A missão de 12 etapas foi executada. **O objetivo de 30 FPS estáveis NÃO foi atingido** na cena crítica (floresta: 17-18 FPS). A causa raiz foi identificada e é **irreversível no runtime**.

### Causa Raiz
**22.521 draw calls por frame** na floresta — conteúdo do jogo (vegetação com milhares de meshes individuais). O runtime traduz corretamente; o jogo simplesmente submete geometria demais para o Adreno 740 processar em 33ms.

### O que foi feito
- ✅ Auditoria completa do código (00_CODE_AUDIT.md)
- ✅ Baseline estabelecido (01_BASELINE.md)
- ✅ Turnip diagnosticado (bloqueado, 02_TURNIP_DIAGNOSIS.md)
- ✅ Uploads investigados (sintoma, não causa, 03_TEXTURE_UPLOAD_AUDIT.md)
- ✅ Sincronização auditada (necessária, não redundante, 04_SYNC_ANALYSIS.md)
- ✅ Render passes auditados (1.7k, dirigidos pelo jogo, 05_RENDER_PASS_AUDIT.md)
- ✅ Shader cache verificado (funcionando, 06_SHADER_CACHE_AUDIT.md)
- ✅ Resolução/FSR1 validados (07_08_RESOLUTION_FRAMING.md)
- ✅ Surface loss documentado (09_SURFACE_LOSS.md)

### Otimizações Aplicadas (v0053, v0058)
1. Dynamic flag reset após 120 frames (reduz hash desnecessário)
2. SMAA default OFF no Android (economiza 6 passes fullscreen)
3. GCOPY_TAB 4096→16384 (reduz thrash do cache)
4. Shader workers 2→3 (padrão LASRecomp-Android-Plus)

**Resultado:** FPS médio 26.7 (era ~24 antes das otimizações). Ganho modesto porque o gargalo é contagem de draws, não overhead do runtime.

---

## Arquivos Alterados

| Arquivo | Mudança | Motivo |
|---------|---------|--------|
| `src/runtime/nvn/vulkan/textures.c` | Campo `dyn_stable`, reset após 120 frames | Evitar hash full permanente |
| `src/runtime/settings.c` | SMAA default OFF no Android | Economizar 6 passes |
| `src/runtime/nvn/vulkan/exec.c` | GCOPY_TAB 16384, workers=3 | Reduzir thrash, padrão referência |
| `src/runtime/nvn/vulkan/device.c` | Log `[vk] active GPU` | Diagnóstico Turnip |

---

## Gargalos Não Resolvidos

| # | Gargalo | Por que não resolvido |
|---|---------|----------------------|
| 1 | 22k draws na floresta | Conteúdo do jogo; exigiria merging de geometria |
| 2 | Turnip não carrega | `adrenotools` falha silenciosamente; sem fonte para debug |
| 3 | Texto invisível | Bug no `cbr` do tradutor; v0057 testou sem `min()`, sem mudança |
| 4 | Surface loss | Requer teste em aparelho para validar fix |

---

## Recomendações

1. **Não perseguir 30 FPS na floresta** — é limitação do conteúdo, não do port
2. **Focar no texto invisível** — bug real que afeta jogabilidade
3. **Turnip:** Aguardar atualização ou obter fonte do adrenotools
4. **Testes:** Necessário S23 para validar surface loss e comparative tests

---

## Critério de Sucesso

**NÃO ATINGIDO.** O objetivo de 30 FPS estáveis requer redução de draws, impossível no runtime sem modificar o jogo.
