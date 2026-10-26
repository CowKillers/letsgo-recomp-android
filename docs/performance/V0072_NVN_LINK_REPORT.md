# Relatório v0072 — IDENTIFICAR O DRAW REAL DOS GLIFOS

**Data:** 2026-10-21
**Build:** v0072 (versionCode 1792589807)
**Base:** v0071

---

## Contexto: Falha da v0071

A v0071 aplicou magenta nos shaders `33b33498a501870e` e `8b8aa04c466912da`,
mas o teste mostrou:
- Frame 1105: caixa de diálogo vazia
- Shader no draw: `6443b63b341c73ca` (DIFERENTE dos patchados!)
- Nenhum pixel magenta visível

**Conclusão:** Os shaders patchados não eram os do texto. O draw real usa
o shader NVN `6443b63b341c73ca`.

---

## MISSÃO 1 — Vínculo NVN→GLSL ✅ IMPLEMENTADO

### Mudança na Assinatura
`vk_glsl_to_spirv()` agora recebe `uint64_t nvn_hash`:
```c
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, 
                           size_t *nwords, uint64_t nvn_hash);
```

### Log de Mapeamento
Quando `nvn_hash != 0`, loga:
```
[shader] NVN2GLSL: nvn=6443b63b341c73ca -> glsl=<hash> stage=1 name=fs
```

Isso revela o hash GLSL (nome do arquivo) correspondente ao hash NVN do log.

### Call Sites Atualizados
- `exec.c:1222`: `vk_glsl_to_spirv(..., p->code[0])` (vertex NVN)
- `exec.c:1223`: `vk_glsl_to_spirv(..., p->code[1])` (fragment NVN)
- `exec.c:2320`: `vk_glsl_to_spirv(..., 0)` (compute, sem NVN)
- `shaders.c:283`: `vk_glsl_to_spirv(..., 0)` (interno)

---

## MISSÃO 2 — Magenta no Pipeline Comprovado ✅ IMPLEMENTADO

### Alvo Correto
O `TEXT_DIAG` agora verifica o hash **NVN** (não mais o GLSL):
```c
if (text_diag && stage == 1 && nvn_hash == 0x6443b63b341c73caULL)
```

### Garantia de Recompilação
O código diagnóstico modifica o source GLSL e **recomputa o hash**:
```c
h = src_hash(effective_src, stage);
```
Isso garante:
- O cache SPIR-V não retorna a versão antiga (hash diferente = miss)
- O pipeline Vulkan não reutiliza o shader não modificado
- O log mostra: `[shader] TEXT_DIAG: applying mode 1 to NVN 6443b63b341c73ca (GLSL <new>)`

### Modo 1 (Atual)
- Magenta opaco `vec4(1.0, 0.0, 1.0, 1.0)`
- Sem `discard`
- Testa: geometria e rasterização

---

## MISSÃO 3 — Interpretação (Aguardando Teste)

### Se Magenta Aparecer
- Geometria e rasterização OK para o pipeline `6443b63b341c73ca`
- Problema está no `discard`, alpha, ou UVs
- Próximo: Modo 2 (magenta com discard)

### Se Nada Aparecer
- Verificar:
  - O log mostra `TEXT_DIAG: applying mode 1 to NVN 6443b63b341c73ca`?
  - Se não, o shader não foi recompilado (cache de programa?)
  - Contagem de vértices/índices
  - Viewport/scissor efetivos
  - Se o pipeline modificado foi realmente executado

### Não Concluir Prematuramente
Conforme a ordem: "Não concluir que a geometria está incorreta antes de
comprovar que o shader diagnóstico foi vinculado."

---

## Arquivos Modificados

| Arquivo | Alteração |
|---------|-----------|
| `src/runtime/nvn/vulkan/shaders.h` | Assinatura `vk_glsl_to_spirv` com `nvn_hash` |
| `src/runtime/nvn/vulkan/shaders.c` | Log `NVN2GLSL`; `TEXT_DIAG` por NVN hash; usa `effective_src` |
| `src/runtime/nvn/vulkan/exec.c` | Passa `p->code[0]`/`p->code[1]`/`0` nas chamadas |
| `apk/AndroidManifest.xml` | `versionCode` 1792589807 |

---

## Testes

### Compilação v0072
- ✅ `SMOKE_OK`
- ✅ `RUNTIME_LINK_OK`
- ✅ `APK_OK` (373 MB)
- ✅ Assinatura SHA-256 correta

### Para Testar no S23
1. Instale v0072
2. Abra um diálogo
3. Verifique no log: `[shader] NVN2GLSL: nvn=6443b63b341c73ca -> glsl=<X>`
   - Isso revela o arquivo GLSL real do texto!
4. Verifique: `[shader] TEXT_DIAG: applying mode 1 to NVN 6443b63b341c73ca`
   - Confirma que o diagnóstico foi aplicado
5. **Se ver magenta**: geometria OK → investigamos discard/alpha/UV
6. **Se não ver nada**: o pipeline não foi vinculado → investigamos cache

### Mapa de Hashes (a completar com o teste)
- NVN (fshash): `6443b63b341c73ca`
- GLSL (arquivo): `<a descobrir via NVN2GLSL>`
- SPIR-V: `<hash do SPIR-V modificado>`
