# Relatório v0073 — CORREÇÃO DO VÍNCULO FSHASH

**Data:** 2026-10-21
**Build:** v0073 (versionCode 1792595890)
**Base:** v0072

---

## Bug Encontrado na v0072

A v0072 não aplicou o magenta porque o `TEXT_DIAG` comparava o hash errado.

### Causa Raiz
- `p->code[1]` é o **ENDEREÇO** do shader Maxwell na memória guest, não o hash
- O `fshash` do log (`6443b63b341c73ca`) é calculado via:
  ```c
  hmix(1469598103934665603ull, (fs_addr + 0x30), 0x200)
  ```
- A v0072 comparava `p->code[1] == 0x6443b63b341c73ca`, que nunca é verdade
  (endereço ≠ hash)

### Evidência
- Nenhum GLSL no cache continha o override magenta
- Os shaders `33b33498a501870e` e `8b8aa04c466912da` mantiveram código original
- O `TEXT_DIAG` nunca disparou para o shader real

---

## Correção v0073

### Cálculo do Fshash em `compile_program`
```c
uint64_t vs_fshash = p->code[0] ? hmix(1469598103934665603ull, 
    (const void *)(uintptr_t)(p->code[0] + 0x30), 0x200) : 0;
uint64_t fs_fshash = p->code[1] ? hmix(1469598103934665603ull, 
    (const void *)(uintptr_t)(p->code[1] + 0x30), 0x200) : 0;
uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv, vs_fshash);
uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf, fs_fshash) : NULL;
```

### Log NVN2GLSL Atualizado
```
[shader] NVN2GLSL: fshash=6443b63b341c73ca -> glsl=<hash> stage=1 name=fs
```
Agora o `fshash` bate com o do `[font-draw]`.

### TEXT_DIAG Corrigido
```c
if (text_diag && stage == 1 && nvn_hash == 0x6443b63b341c73caULL)
```
Onde `nvn_hash` é agora o fshash (não mais o endereço).

### Pipeline Log com Fshash
O `pipeline_create.log` agora inclui:
```
vs_fshash=<hash> fs_fshash=<hash>
```
Permite vincular o pipeline ao draw via fshash.

---

## Cadeia Completa (para o teste)

1. **NVN**: Endereço `p->code[1]` → fshash `6443b63b341c73ca`
2. **GLSL**: `NVN2GLSL: fshash=6443b63b341c73ca -> glsl=<X>`
3. **SPIR-V**: Compilado do GLSL modificado (magenta), hash diferente
4. **Pipeline**: `pipeline_create.log` com `fs_fshash=6443b63b341c73ca`
5. **Draw**: `[font-draw] fshash=6443b63b341c73ca`

---

## Arquivos Modificados

| Arquivo | Alteração |
|---------|-----------|
| `src/runtime/nvn/vulkan/exec.c` | Calcula fshash em `compile_program`; forward decl `hmix`; fshash no pipeline log |
| `src/runtime/nvn/vulkan/shaders.c` | Log `NVN2GLSL` com fshash; `TEXT_DIAG` usa fshash |
| `src/runtime/nvn/vulkan/shaders.h` | (sem mudança, assinatura já tinha `nvn_hash`) |
| `apk/AndroidManifest.xml` | `versionCode` 1792595890 |

---

## Testes

### Compilação v0073
- ✅ `SMOKE_OK`
- ✅ `RUNTIME_LINK_OK`
- ✅ `APK_OK` (373 MB)
- ✅ Assinatura SHA-256 correta

### Para Testar no S23
1. Instale v0073
2. Abra um diálogo
3. No log, confirme:
   - `[shader] NVN2GLSL: fshash=6443b63b341c73ca -> glsl=<X>`
   - `[shader] TEXT_DIAG: applying mode 1 to fshash 6443b63b341c73ca`
4. **Se ver magenta**: o shader real foi patchado → geometria OK
5. **Se não ver**: me envie o log para diagnóstico
