# Revisão Aprofundada — Texto Invisível no Let's Go

## Diagnóstico Definitivo

### O que foi provado
1. **Atlas tem glifos**: Dump do atlas 2048x1024 mostra "Route 2", "Get ready" — rasterização funciona
2. **Binding correto**: Log `[font-bind]` confirma que o diálogo vincula o atlas ao fragment shader
3. **Hash descartado**: Teste A/B com fullhash forçado não fez texto aparecer
4. **Tradução descartada**: Executável `main` é byte-idêntico ao original inglês
5. **Draws acontecem**: 0 skipped nos logs

### Onde está o bug
O problema está no **tradutor Maxwell→GLSL** (`src/runtime/nvn/maxwell/translate.c`), especificamente no padrão **"corner-indexed texture-coordinate load"**.

O Wonder teve o **mesmo sintoma** (diálogo invisível) e a nota no PROGRESS.md diz:
> "the glyph vertex shader's corner-indexed texture-coordinate load was only partly resolved by the offline translator"

### O padrão "corner-indexed"
Para renderizar glifos, o vertex shader não armazena UVs no vertex buffer. Em vez disso:
1. Usa o `corner_index` (0-3) do vértice
2. Carrega o UV de uma tabela na constant buffer: `LDC R4, c[i][R4 + x]`
3. Onde R4 contém `corner_index * 16` (offset em bytes)

### O bug no tradutor
O handler LDC em `translate.c:899-915` gera:
```glsl
cbr(cbN, (dyn) + off)
```

Onde `cbr` é:
```glsl
#define cbr(cb, off) cb[min((off) >> 4, uint(cb.length() - 1))][((off) >> 2) & 3u]
```

**O `min()` está mascarando um índice fora dos limites.** Se o cálculo do `off` estiver errado:
- Todos os cantos pegam o último elemento da tabela (clamp)
- Todos os vértices têm o mesmo UV
- O quad tem área zero no espaço UV
- O fragment shader amostra um único texel (vazio) → texto invisível

### Por que "only partly resolved"
O fix atual (linhas 906-908) trata o caso onde o registrador de endereço é também destino:
```c
// LDC.64 R4, c[i][R4 + x]: the address register is one of the destinations.
bool overlap = dyn && n > 1 && a >= d && a < d + n;
```

Mas pode haver outros padrões de "corner-indexed" não cobertos, ou o cálculo do índice dinâmico pode estar errado para este shader específico.

## Solução Proposta

### Opção 1: Remover o clamp (diagnóstico)
Remover o `min()` do `cbr` para expor o índice real:
```glsl
#define cbr(cb, off) cb[(off) >> 4][((off) >> 2) & 3u]
```
- Se crashar: confirma índice fora dos limites
- Se texto aparecer: o clamp era o problema

### Opção 2: Log do índice (diagnóstico)
Modificar o `cbr` para logar quando o clamp é acionado, identificando o shader problemático.

### Opção 3: Fix no cálculo do índice (correção)
Investigar o shader específico do diálogo e corrigir o cálculo do `dyn` no handler LDC.

## Referências
- Wonder PROGRESS.md: "glyph vertex shader's corner-indexed texture-coordinate load"
- HISTORICO.md linha 125: Documenta o fix parcial do LDC overlap
- `translate.c:899-915`: Handler LDC
- `translate.c:1747`: Definição do `cbr` com `min()`

## Status
Aguardando JDK para buildar e testar a Opção 1.
