# V0059_TEXT_DEBUG.md

## Relatório v0059-text-debug.apk
**Data:** 2026-10-21
**Objetivo:** Diagnosticar texto invisível (MISSÃO C)

---

## Instrumentação Implementada

### ETAPA 1: Rastreamento do Atlas ✅
- **[font-bind]** em `exec.c:2152`: Loga quando ATLAS2048 (2048x1024 R8) é vinculada
- Mostra: endereço, dimensões, stage, slot do descriptor
- **Resultado conhecido:** Atlas contém glifos ("Route 2", "Get ready" no dump)

### ETAPA 2: Instrumentação dos Draws ✅
- **[font-draw]** em `exec.c:2424`: Loga quando draw usa o atlas
- Mostra: draw ID, frame ID, indexed, program pointer
- **Limite:** 10 draws (evita spam, ETAPA 5)

### ETAPA 6: Adrenotools ✅
- `ADT_LOG` adicionado em `wonder-recomp/adrenotools-src/src/driver.cpp`
- Logs em cada um dos 5 pontos de falha silenciosa
- **Nota:** Biblioteca instrumentada NÃO está na v0059 (requer rebuild do adrenotools)

---

## As 7 Perguntas (ETAPA 8)

| # | Pergunta | Status | Evidência |
|---|----------|--------|-----------|
| 1 | Atlas contém glifos? | ✅ SIM | `atlas_dump` mostrou texto legível |
| 2 | Draw usa textura correta? | ✅ SIM | `[font-bind]` confirma ATLAS2048 no slot 0 |
| 3 | UVs apontam para glifos? | ❓ PENDENTE | Requer teste no aparelho com v0059 |
| 4 | Shader produz saída? | ❓ PENDENTE | Requer teste no aparelho |
| 5 | Blend/depth/scissor ocultam? | ❓ PENDENTE | Requer teste no aparelho |
| 6 | Framebuffer recebe glifos? | ❓ PENDENTE | Requer GPU readback no aparelho |
| 7 | Passe posterior remove? | ❓ PENDENTE | Requer teste no aparelho |

---

## Hipóteses Descartadas

- **H1 (hash amostral):** v0055 testou `always_full=1` → sem mudança
- **H2 (cbr min):** v0057 removeu `min()` → sem mudança

## Hipóteses Restantes

- **H3:** UVs zerados no vertex shader
- **H4:** Alpha/blend incorreto
- **H5:** Clipping/descarte
- **H6:** Slot errado (mas [font-bind] mostra slot 0)

---

## APK

**Arquivo:** `LetsGoRecomp-v0059-text-debug.apk` (Drive)
**VersionCode:** 1792565403
**Tamanho:** 373 MB

**Preserva:**
- ✅ Gameplay, texturas, modelos
- ✅ Controles, áudio, salvamentos
- ✅ Resolução e proporção
- ✅ Correções v0053/v0058

**Adiciona:**
- `[font-bind]` (20 logs)
- `[font-draw]` (10 logs)

---

## Instruções de Teste

1. Instale a v0059
2. Abra um diálogo (fale com NPC)
3. Capture o log com `adb logcat | grep -E "font-bind|font-draw"`
4. Envie o log

**O que procurar:**
- `[font-bind]` aparece? (atlas vinculada)
- `[font-draw]` aparece? (draw executado)
- Se ambos aparecem mas sem texto visível → problema no shader/UV/blend
- Se `[font-bind]` aparece mas `[font-draw]` não → draws estão sendo pulados
