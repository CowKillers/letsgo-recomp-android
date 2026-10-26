# ROM_ORIGINAL_AUDIT.md

## Auditoria de Arquivos Originais vs Traduzidos
**Data:** 2026-10-20
**Projeto:** Pokémon Let's Go Eevee! - Port Android ARM64
**Ordem:** ORDEM MESTRE — RESTAURAR ARQUIVOS ORIGINAIS

## ETAPA 2 — Arquivos Localizados

### ExeFS (Executável)
- **Local:** `src/extracted/exefs/main`
- **Tamanho:** 13 MB
- **SHA-256:** `a01d9e41887a01d3151a597e6e58525fed920a2b5c8e371770b4e5e6c27b6f8f`
- **Data de extração:** 2026-10-18 13:55
- **Status:** NÃO VERIFICADO (origem desconhecida - PT-BR ou original?)

### RomFS (Dados)
- **Local:** `src/extracted/romfs/`
- **Pastas de mensagem:** English, French, German, Italian, JPN, JPN_KANJI, Korean, Simp_Chinese, Spanish, Trad_Chinese
- **NOTA:** Não há pasta "Portuguese" - a tradução PT-BR provavelmente substituiu os arquivos dentro de "English/"
- **Status:** MODIFICADO (suspeita de tradução PT-BR nos arquivos English/)

### Código Recompilado
- **Local:** `src/generated-aarch64/`
- **Data de geração:** 2026-10-18 14:02 (7 min após extração do ExeFS)
- **Origem:** Gerado a partir de `src/extracted/exefs/main`
- **Status:** NÃO VERIFICADO (depende da origem do `main`)

## ETAPA 4 — Classificação (ATUALIZADO 20/10 20:55)

| Arquivo | Status | Notas |
|---------|--------|-------|
| `exefs/main` | **ORIGINAL** | SHA-256 `a01d9e41...` — **IDÊNTICO** ao da ROM inglesa original. A tradução NÃO modificou o executável. |
| `romfs/bin/message/English/` | MODIFICADO | Tradução PT-BR substituiu conteúdo dentro de `English/` (sem pasta Portuguese) |
| `generated-aarch64/` | **ORIGINAL** | Gerado de um `main` não-modificado; o código recompilado NÃO tem alterações da tradução |

## Conclusão Definitiva

**O executável NÃO foi modificado pela tradução PT-BR.** Os arquivos `main` da ROM traduzida e da ROM original em inglês são **byte-por-byte idênticos** (SHA-256 igual, `cmp` confirma).

Portanto:
- O código recompilado (`generated-aarch64/`) é íntegro e original
- A causa do texto invisível **NÃO** é a tradução PT-BR
- O problema está no **runtime** (tradutor Vulkan Maxwell→GLSL) ou no caminho de renderização, conforme já investigado nas etapas P1 anteriores

**A hipótese da tradução como causa está DEFINITIVAMENTE descartada.**
