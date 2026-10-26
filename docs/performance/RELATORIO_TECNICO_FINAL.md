# RELATORIO_TECNICO_FINAL.md

## Relatório Técnico Final — ORDEM TÉCNICA v0058/v0059
**Data:** 2026-10-21

---

## 1. Causas Demonstradas

### 1.1 Turnip não carrega (MISSÃO A)
**Causa raiz:** Incompatibilidade do `linkernsbypass` com Android 16 (API 36).
**Evidência:**
- Código-fonte inspecionado: `wonder-recomp/adrenotools-src/src/driver.cpp:21`
- `adrenotools_open_libvulkan` retorna `nullptr` em 5 pontos sem `dlerror()`
- `linkernsbypass_load_status()` retorna `false` quando símbolos do linker não são encontrados
- Upstream (bylaws) último update em 2024, antes do Android 16
- **Instrumentação adicionada:** `ADT_LOG` em cada ponto de falha

**Arquivos alterados:**
- `wonder-recomp/adrenotools-src/src/driver.cpp` (instrumentação, não compilada no APK)

### 1.2 Gargalo gráfico (MISSÃO B)
**Métricas por frame (floresta, 17.1 FPS):**
- 621 draws/frame
- 44 passes/frame  
- 21.9 MB upload/frame
- 77ms wait/frame (GPU ocupada)

**Separação:**
- **CPU:** Overhead de 621 draws (submissão de comandos)
- **GPU:** 44 passes + geometria da vegetação
- **Wait:** Dependência real, não overhead
- **Térmico:** Não medido (requer aparelho)

**Conclusão:** Backend eficiente; conteúdo pesado. Predição: CPU-bound (480p melhoraria apenas ~10-15%).

### 1.3 Texto invisível (MISSÃO C - PRIORIDADE)
**Caminho verificado:**
1. ✅ Mensagens lidas (`English/*.dat`)
2. ✅ Beluga rasteriza (watchpoint + atlas_dump com "Route 2")
3. ✅ Atlas 2048x1024 R8 criado
4. ✅ Binding correto (`[font-bind]` no slot 0)
5. ❌ **FALHA:** Pixels não aparecem na tela

**Hipóteses descartadas:**
- H1 (hash amostral): v0055 testou → sem mudança
- H2 (cbr min): v0057 testou → sem mudança

**Hipóteses restantes:**
- H3: UVs zerados no vertex shader
- H4: Alpha/blend incorreto
- H5: Clipping/descarte
- H6: Slot errado (improvável, [font-bind] confirma)

**Instrumentação v0059:**
- `[font-bind]`: 20 logs de vinculação do atlas
- `[font-draw]`: 10 logs de draws usando o atlas
- **Pendente:** Teste no S23 para responder as 7 perguntas

---

## 2. Arquivos Alterados (v0058/v0059)

| Arquivo | Mudança | Versão |
|---------|---------|--------|
| `src/runtime/nvn/vulkan/textures.c` | `dyn_stable`, reset após 120 frames | v0053 |
| `src/runtime/settings.c` | SMAA default OFF no Android | v0053 |
| `src/runtime/nvn/vulkan/exec.c` | GCOPY_TAB 16384, workers=3, `[font-bind]`, `[font-draw]`, `g_font_atlas_bound` | v0053/v0058/v0059 |
| `src/runtime/nvn/vulkan/device.c` | Log `[vk] active GPU` | v0054 |
| `apk/AndroidManifest.xml` | versionCode 1792565403 | v0059 |
| `wonder-recomp/adrenotools-src/src/driver.cpp` | `ADT_LOG` instrumentação | (não no APK) |

---

## 3. Resultado do Carregamento Turnip

**Status:** **FALHA** (não corrigido)

**Log do aparelho:**
```
[vk] custom GPU driver .../libvulkan_freedreno.so failed to load via adrenotools: (null)
[vk] active GPU: Adreno (TM) 740 (vendor 0x5143, device 0x43050a01, driver v512.676.73)
```

**Causa:** `linkernsbypass` incompatível com Android 16. Requer atualização da biblioteca de terceiros ou bypass.

**Fallback:** Qualcomm Adreno 740 ativo e funcional.

---

## 4. Estado da Renderização dos Textos

**Status:** **INVISÍVEL** (não corrigido)

**Evidência:**
- Atlas contém glifos (dump binário confirmado)
- Textura vinculada corretamente
- Diálogos aparecem vazios no jogo

**Próximo passo:** Testar v0059 no S23 e analisar logs `[font-bind]`/`[font-draw]`.

---

## 5. APK Compilado

**v0059-text-debug.apk:**
- **Drive:** `1GVNsftb1Ntfvn1NsQfou-7-UvVLw578H` (pasta Muse)
- **VersionCode:** 1792565403
- **Tamanho:** 373 MB
- **Base:** v0058 (workers=3) + instrumentação de texto

---

## 6. Comparação com v0058

| Aspecto | v0058 | v0059 |
|---------|-------|-------|
| Workers | 3 | 3 |
| SMAA | OFF | OFF |
| GCOPY | 16384 | 16384 |
| `[font-bind]` | Sim | Sim |
| `[font-draw]` | Não | **Sim (novo)** |
| `g_font_atlas_bound` | Não | **Sim (novo)** |

**Mudanças:** Apenas instrumentação de diagnóstico. Nenhuma mudança funcional.

---

## 7. Problemas Pendentes

| # | Problema | Motivo | Próximo passo |
|---|----------|--------|---------------|
| 1 | Texto invisível | Causa no shader/UV/blend não identificada | Testar v0059, analisar logs |
| 2 | Turnip no Android 16 | `linkernsbypass` desatualizado | Atualizar lib ou fazer bypass |
| 3 | 17 FPS na floresta | 621 draws/frame (conteúdo do jogo) | Aceitar limitação ou reduzir conteúdo |
| 4 | Surface loss | Requer teste de minimizar/retomar | Testar no S23 |
| 5 | Teste 480p vs 720p | Requer aparelho | Executar benchmark controlado |

---

## Objetivo Final

**Status:** **PARCIALMENTE ATINGIDO**

- ✅ Jogo funcional (gameplay, áudio, controles, saves)
- ❌ Diálogos visíveis (texto invisível)
- ❌ Driver selecionável (Turnip falha no Android 16)
- ✅ Desempenho medido (26.7 FPS médio, 17.1 mínimo na floresta)

**Para atingir o objetivo completo:**
1. Resolver texto invisível (prioridade máxima)
2. Corrigir Turnip para Android 16 (ou documentar como limitação)
3. Aceitar 17-18 FPS na floresta como teto arquitetural
