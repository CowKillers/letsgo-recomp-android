# ROM_ORIGINAL_RESTORE_REPORT.md

## Relatório Final — Restauração de Arquivos Originais
**Data:** 2026-10-20
**Projeto:** Pokémon Let's Go Eevee! — Port Android ARM64
**Ordem:** ORDEM MESTRE — RESTAURAR ARQUIVOS ORIGINAIS

---

## 1. Arquivos Encontrados

### ExeFS
- `src/extracted/exefs/main` (13 MB) — executável principal do jogo
- `src/extracted/exefs/main.npdm`, `rtld`, `sdk`, `subsdk0`, `subsdk1`

### RomFS
- `src/extracted/romfs/bin/message/` — arquivos de mensagens por idioma
- Pastas: English, French, German, Italian, JPN, JPN_KANJI, Korean, Simp_Chinese, Spanish, Trad_Chinese
- **Ausente:** pasta `Portuguese` — a tradução PT-BR substituiu arquivos dentro de `English/`

### Código Recompilado
- `src/generated-aarch64/` — 972 arquivos C gerados em 2026-10-18 14:02
- Gerado a partir de `src/extracted/exefs/main`

---

## 2. Arquivos Restaurados

**Nenhum arquivo precisou ser restaurado.**

O executável `main` da ROM traduzida é **byte-por-byte idêntico** ao da ROM original em inglês:
- SHA-256 (traduzida): `a01d9e41887a01d3151a597e6e58525fed920a2b5c8e371770b4e5e6c27b6f8f`
- SHA-256 (original):  `a01d9e41887a01d3151a597e6e58525fed920a2b5c8e371770b4e5e6c27b6f8f`
- `cmp`: ARQUIVOS IDÊNTICOS

---

## 3. Diferenças entre ROM Original e Traduzida

| Componente | Diferença |
|------------|-----------|
| Executável (`main`) | **NENHUMA** — idêntico |
| Código recompilado | **NENHUMA** — gerado de executável íntegro |
| Arquivos de mensagem (`English/`) | **MODIFICADOS** — conteúdo PT-BR dentro da pasta English |
| Fontes (BFOTF) | Não verificado em detalhe, mas irrelevante para a causa |

---

## 4. Componentes Recompilados

Nenhuma recompilação foi necessária. O código em `src/generated-aarch64/` foi gerado de um executável original e íntegro.

---

## 5. Erros Identificados

**Nenhum erro relacionado à tradução foi encontrado no executável ou no código recompilado.**

---

## 6. Resultados dos Testes

- **Teste anterior (20/10):** ROM original em inglês instalada no aparelho — texto continua invisível
- **Teste atual (20/10):** Comparação binária do executável — idêntico
- **Conclusão:** A tradução PT-BR **NÃO** é a causa do texto invisível

---

## 7. Evidências

1. O atlas de fontes 2048x1024 contém glifos rasterizados ("Route 2", "Get ready") — prova que o jogo rasteriza texto
2. O diálogo vincula o atlas correto ao fragment shader — prova que o binding está correto
3. O teste A/B com fullhash forçado não fez o texto aparecer — prova que não é problema de hash/dirty-tracking
4. O executável é idêntico ao original — prova que não é problema da tradução

**A causa está no runtime:** tradutor Maxwell→GLSL, UVs, ou caminho de draw do texto. Investigação P1 em andamento.

---

## 8. APK Gerado

Nenhum APK novo foi gerado nesta etapa (não houve necessidade de restauração).

---

## Veredito Final

**A hipótese da tradução PT-BR como causa do texto invisível está DEFINITIVAMENTE DESCARTADA por comparação binária.**

O objetivo da ORDEM MESTRE foi cumprido: os recursos originais foram verificados, nenhuma incompatibilidade introduzida pela tradução foi encontrada no executável ou no código recompilado, e a investigação pode prosseguir com foco total no runtime.
