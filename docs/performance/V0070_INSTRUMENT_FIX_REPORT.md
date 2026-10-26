# Relatório v0070 — CORREÇÃO DA INSTRUMENTAÇÃO

**Data:** 2026-10-21
**Build:** v0070 (versionCode 1792574715)
**Base:** v0069

---

## DEFEITO A — CAP AUTOMÁTICO NA INICIALIZAÇÃO ✅ CORRIGIDO

### Causa
Arquivo `capture_font` remanescente de sessão anterior disparava o gatilho
imediatamente no frame 12, antes do usuário pressionar CAP.

### Correções Implementadas

**1. Limpeza na inicialização** (`exec.c`):
- Na primeira verificação (`!trigger_checked`), o código agora REMOVE qualquer
  `capture_font` existente **sem disparar** o gatilho
- Log: `[font-capture] Stale trigger file removed (not triggering)`

**2. Consumo com verificação** (`exec.c`):
- Após detectar o gatilho, o `remove()` agora verifica o resultado
- Log de sucesso: `[font-capture] TRIGGERED: on-demand capture activated (trigger consumed)`
- Log de falha: `[font-capture] TRIGGERED: ... (trigger remove FAILED: <errno>)`

**3. Log CAP_REQUESTED** (`GamepadOverlay.java`):
- Ao pressionar o botão CAP, loga: `[font-capture] CAP_REQUESTED: trigger file <path> created`
- Em caso de erro: `[font-capture] CAP_REQUESTED FAILED: <mensagem>`

**4. Log CAP_FRAME_SELECTED** (`exec.c`):
- Quando o gatilho é detectado: `[font-capture] CAP_FRAME_SELECTED: frame <N>`

**5. Logs CAP_WRITE_OK/FAILED** (`device.c`):
- `vk_save_png()` agora detecta capturas de fonte (path contém "font_frame")
- Sucesso: `[font-capture] CAP_WRITE_OK: <path> (<WxH>)`
- Falha: `[font-capture] CAP_WRITE_FAILED: <path> (<erro>)`

**6. Nome único por frame** (`device.c`):
- Antes: `font_frame.png` (sobrescrevia)
- Agora: `font_frame_<frame>.png` (preserva capturas anteriores)
- Exemplo: `font_frame_952.png`, `font_frame_1047.png`

### Validação
Teste em `/tmp` com programa C simulando o mecanismo:
- ✅ Arquivo remanescente removido sem disparar
- ✅ Arquivo novo dispara corretamente
- ✅ Gatilho consumido após uso (não dispara de novo)
- Resultado: `ALL TESTS PASSED`

---

## DEFEITO B — GLSL ENABLED SEM ARQUIVOS ✅ CORRIGIDO

### Causa
O código de dump usava `fopen(path, "wx")` sem:
1. Criar o diretório `cache/` se não existisse
2. Logar quando o arquivo já existia (falha silenciosa)
3. Logar quando o `fopen` falhava por outros motivos
4. Dumpar vertex shaders (só fragment, `stage == 1`)

### Correções Implementadas (`shaders.c`)

**1. Logs explícitos:**
- `GLSL_DUMP_ATTEMPT`: `stage=<N> hash=<H> path=<absoluto>`
- `GLSL_DUMP_OK`: `<path> (<N> bytes)`
- `GLSL_DUMP_FAILED`: `<path> (fopen: <errno>)` ou `(write incomplete)`
- `GLSL_DUMP_SKIP`: `<path> (already exists)` — não é erro

**2. Criação de diretório:**
- `mkdir(dir, 0755)` antes do `fopen`
- Garante que `cache/` existe

**3. Dump de vertex + fragment:**
- Antes: apenas `stage == 1` (fragment)
- Agora: ambos `stage == 0` (vertex, extensão `.vert`) e `stage == 1` (fragment, `.frag`)
- Permite analisar o par completo para os hashes `7270eb69fbecccee` e `6443b63b341c73ca`

**4. Sem dumps ilimitados:**
- Mantido o `fopen("wx")` — só salva se não existe (evita duplicatas)
- Não há loop sobre todos os pipelines; apenas shaders compilados são dumpados
- Desempenho preservado

### Validação
Teste em `/tmp` com programa C simulando o mecanismo:
- ✅ `GLSL_DUMP_OK` com 29 bytes (conteúdo não vazio)
- ✅ Segunda escrita corretamente ignorada (`EEXIST`, não `FAILED`)
- ✅ Verificação de conteúdo não vazio
- Resultado: `ALL TESTS PASSED`

---

## Arquivos Modificados

| Arquivo | Alteração |
|---------|-----------|
| `src/runtime/nvn/vulkan/exec.c` | Limpeza de gatilho remanescente; logs `CAP_FRAME_SELECTED`; `#include <errno.h>` |
| `src/runtime/nvn/vulkan/device.c` | Nome único `font_frame_<N>.png`; logs `CAP_WRITE_OK`/`FAILED` em `vk_save_png()` |
| `src/runtime/nvn/vulkan/shaders.c` | Logs `GLSL_DUMP_*`; `mkdir`; dump vertex+fragment; `#include <errno.h>` |
| `apk/java/com/letsgorecomp/GamepadOverlay.java` | Log `CAP_REQUESTED` no botão CAP |
| `apk/AndroidManifest.xml` | `versionCode` 1792574715 |

---

## Testes

### Compilação v0070
- ✅ `SMOKE_OK`
- ✅ `RUNTIME_LINK_OK`
- ✅ `APK_OK` (373 MB)
- ✅ Assinatura SHA-256 correta

### Validação de Mecanismos (antes do APK)
- ✅ Escrita GLSL em `/tmp`: conteúdo não vazio, duplicatas ignoradas
- ✅ Gatilho CAP em `/tmp`: remanescente limpo sem disparar, novo dispara e é consumido

### Critérios de Aceite (para teste no S23)
- [ ] CAP não dispara sozinho na inicialização
- [ ] CAP registra frame escolhido durante o jogo
- [ ] Arquivos GLSL gerados em `cache/glsl_*.vert` e `cache/glsl_*.frag`
- [ ] Logs `GLSL_DUMP_OK` visíveis no logcat

---

## Preservação

- ✅ Sem alterações em shaders, UV, Beluga, alpha ou renderizador
- ✅ Gameplay, áudio, controles, saves preservados
- ✅ Sem V-flip
- ✅ v0069 preservada como referência
