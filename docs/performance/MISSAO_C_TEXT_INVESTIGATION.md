# MISSAO_C_TEXT_INVESTIGATION.md

## MISSÃO C: Texto Invisível — Investigação
**Data:** 2026-10-21
**Prioridade:** MÁXIMA
**Status:** Em investigação

---

## Caminho Completo (verificado)

| Etapa | Status | Evidência |
|-------|--------|-----------|
| 1. Arquivo de mensagem | ✅ | `bin/message/English/common/*.dat` lidos |
| 2. Beluga gera glifos | ✅ | Watchpoint disparou em 0x729c3aa92a |
| 3. Atlas de fontes | ✅ | `atlas_dump` mostrou "Route 2", "Get ready" |
| 4. Textura R8 2048x1024 | ✅ | Criada em 0x729c1bc000 |
| 5. Descriptor binding | ✅ | `[font-bind]` log: bound to stage 1, slot 0 |
| 6. Shader amostra | ❌ | **FALHA AQUI** — texto invisível |
| 7. Framebuffer | ❌ | Nada renderizado |

---

## Hipóteses Testadas

### H1: Hash amostral (v0055)
- **Teste:** `always_full = 1` em textures.c
- **Resultado:** "Não temos texto" — **DESCARTADA**

### H2: Macro `cbr` com `min()` (v0057)
- **Teoria:** `min()` mascarava índice fora dos limites, todos os cantos pegavam mesmo UV
- **Teste:** Removido `min()` → `cb[((off) >> 4)][((off) >> 2) & 3u]`
- **Resultado:** "Não mudou nada" — **DESCARTADA**
- **Implicação:** O índice NÃO estava fora dos limites; o `min()` não era o problema

---

## Hipóteses Restantes (por probabilidade)

### H3: UVs zerados no vertex shader
- O vertex shader pode não estar escrevendo as coordenadas de textura
- O fragment shader amostra em (0,0) — região vazia do atlas
- **Como testar:** Instrumentar o vertex shader gerado para texto

### H4: Alpha/blending incorreto
- O texto pode estar sendo renderizado mas com alpha=0
- Ou o blend mode está errado (ex: `SRC_ALPHA, ONE` em vez de `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`)
- **Como testar:** Forçar alpha=1.0 no fragment shader do texto

### H5: Clipping/descarte
- Os quads do texto podem estar fora da viewport
- Ou o scissor test está descartando
- **Como testar:** Desabilitar scissor temporariamente

### H6: Textura errada no slot
- O `[font-bind]` mostrou o atlas 2048x1024 no slot 0
- Mas o shader do diálogo pode esperar em outro slot
- **Como testar:** Logar todos os bindings durante o draw do diálogo

---

## Próximos Passos

1. **Instrumentar draws de texto:** Adicionar log quando um draw usa a textura do atlas 2048x1024, mostrando:
   - Vertex count, index count
   - Viewport/scissor
   - Blend state
   - Descriptor set bindings

2. **Capturar o shader:** Dump do vertex/fragment shader usado no draw do diálogo para inspeção manual dos UVs

3. **Teste H4 (alpha):** Modificar o blend para forçar visibilidade

---

## Nota sobre a Tradução

A ORDEM MESTRE provou que o executável é idêntico ao original. A tradução (mod LayeredFS) substitui apenas arquivos em `romfs/bin/message/English/`. Como o texto é invisível mesmo com arquivos originais, **o bug é 100% no runtime**, não nos dados.
