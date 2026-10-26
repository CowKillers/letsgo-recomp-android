# Relatório v0068 — CRASH EM vkCreateGraphicsPipelines

**Data:** 2026-10-21
**Build:** v0068 (versionCode 1792572290)
**Base:** v0067

---

## SITUAÇÃO

Dois crashes SIGSEGV independentes:
- 05:38:06 — primeiro processo
- 05:38:51 — segundo processo

Ambos com acesso ao endereço 0 em `libllvm-qgl.so`, via `CreateQGLCShader`
e `vkCreateGraphicsPipelines` do driver Qualcomm Adreno 740 v512.676.73.

O Turnip não está carregando (fallback para Qualcomm).

O botão CAP funcionou (frame 73). Crash ~26s depois, próximo da transição de menu.

---

## MISSÃO 1 — IDENTIFICAR O PIPELINE ✅ IMPLEMENTADA

### Localização
**Arquivo:** `src/runtime/nvn/vulkan/exec.c`, função `build_pipeline()`, linha ~1751

A chamada `vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe)`
é o ponto principal de criação de pipelines gráficos. É aqui que o driver
Qualcomm compila os shaders via `CreateQGLCShader` e onde ocorre o SIGSEGV.

### Instrumentação Adicionada

**Antes da chamada:** Escreve em `$LARECOMP_DATA_ROOT/pipeline_create.log`:
```
PIPELINE_CREATE_BEGIN id=<N> thread=<TID>
  vs_code=<hash16> fs_code=<hash16>
  ncolor=<N> color0=<fmt> ... depth=<fmt> stencil=<fmt>
  samples=<N> topology=<N>
  blend_on=0x<hex>
  ndyn=<N>
  pcache=<ptr>
  build=v0068
```
Com `fflush()` imediato para garantir persistência antes do crash.

**Depois da chamada:** Escreve:
```
PIPELINE_CREATE_END id=<N> result=<VkResult> duration_ns=<N>
```
Com `fflush()` imediato.

### Identificadores
- `id`: Contador atômico global (`_Atomic uint64_t`), único por tentativa
- `thread`: `pthread_self()` — distingue threads de compilação paralela
- `vs_code`/`fs_code`: Hashes do código Maxwell (`k->prog->code[0]`, `code[1]`)
  - Identificam os shaders guest que geraram o pipeline
- `pcache`: Ponteiro do pipeline cache Vulkan

### Thread Safety
- Contador atômico garante IDs únicos entre threads
- Cada thread escreve no mesmo arquivo, mas com `fopen("a")` + `fflush()` + `fclose()`
  por operação — o modo append do POSIX é atômico para writes pequenos
- Não há mistura de IDs: cada BEGIN/END usa o mesmo `attempt` local

### Como Identificar o Suspeito
Após o crash, examinar `pipeline_create.log`:
1. Encontrar o último `PIPELINE_CREATE_BEGIN` sem `PIPELINE_CREATE_END` correspondente
2. Esse é o pipeline suspeito principal (não causa definitivamente comprovada)
3. Anotar `vs_code` e `fs_code` para correlação com shaders

---

## MISSÃO 2 — VALIDAR SPIR-V ⏳ PARCIAL

### O que foi feito
A instrumentação da MISSÃO 1 registra os hashes Maxwell (`vs_code`, `fs_code`).
Os arquivos SPIR-V estão no cache: `$LARECOMP_DATA_ROOT/cache/spirv/<glsl_hash>.spv`.

### Limitação
O hash GLSL (usado no nome do arquivo SPIR-V) não é o mesmo que o hash Maxwell
(`vs_code`/`fs_code`). A correlação requer:
1. Ativar `LARECOMP_DUMP_FONT_GLSL=1` (já implementado na v0065)
2. Os arquivos `glsl_<hash>.frag` contêm o GLSL
3. O SPIR-V correspondente está em `cache/spirv/<mesmo_hash>.spv`

### Para validação completa (requer teste no dispositivo)
1. Reproduzir o crash com v0068
2. Extrair `pipeline_create.log` do dispositivo
3. Identificar `vs_code`/`fs_code` do pipeline suspeito
4. Localizar os arquivos GLSL/SPIR-V correspondentes
5. Executar `spirv-val --target-env vulkan1.3 <arquivo>.spv`
6. Verificar: integridade, descriptor bindings, interfaces, entrypoints

### Não feito
- Modificação global de shaders: NÃO (conforme ordem)

---

## MISSÃO 3 — DIFERENCIAR REGRESSÃO ⏳ REQUER TESTE

### Comparação v0066 vs v0067
Ambas as builds devem ser testadas na mesma transição de menu, mesmo aparelho/driver.

**Fatores a isolar:**
- A. Pipeline cache atual (padrão)
- B. Pipeline cache novo (renomear `cache/` para `cache.bak`, deixar o jogo recriar)
- C. Captura CAP desativada (não tocar no botão CAP)
- D. Captura CAP ativada (tocar no botão CAP durante o diálogo)

**Hipóteses:**
- Se crash ocorre em A mas não em B: problema no pipeline cache (corrompido?)
- Se crash ocorre em C e D: não relacionado à captura
- Se crash sempre no mesmo `vs_code`/`fs_code`: pipeline específico problemático
- Se crash em v0067 mas não em v0066: regressão introduzida entre as builds

**Diferenças v0066 → v0067:**
- v0067: corrigidos especificadores `%g`/`%u` no `[font-state]`
- v0067: adicionado botão CAP no `GamepadOverlay.java`
- v0067: adicionado log do caminho do gatilho
- Nenhuma alteração no gerador de shaders ou pipeline

**Conclusão preliminar:** As alterações v0066→v0067 são apenas de instrumentação
e UI. É improvável que tenham causado o crash. O crash é mais provavelmente
um pipeline específico que o jogo sempre cria na transição de menu.

---

## MISSÃO 4 — RESTAURAR ESTABILIDADE ⏳ AGUARDANDO IDENTIFICAÇÃO

### Critério
Não corrigir sem identificar o pipeline ou estado incompatível.

### Possíveis causas
1. **Transformação NVN→Vulkan incorreta:** Se o SPIR-V for inválido ou usar
   features não suportadas, corrigir a transformação específica no `translate.c`
2. **Falha do compilador Qualcomm:** Se o SPIR-V for válido mas o driver crashar,
   investigar variante semanticamente equivalente como workaround localizado

### O que NÃO fazer (conforme ordem)
- Substituir shaders complexos por vazios
- Remover efeitos arbitrariamente
- Mascarar SIGSEGV com tratamento genérico
- Ignorar erros de criação de pipeline

### Aceite
Alcançar a área onde v0067 falhava, repetidamente, sem crash e sem perda gráfica.

---

## MISSÃO 5 — TEXTOS ⏳ APÓS ESTABILIZAR

### Observações
- Captura do menu confirmou áreas sem texto visível
- Dois hashes observados em draws com atlas:
  - `7270eb69fbecccee`
  - `6443b63b341c73ca`
- Não assumir que são shaders exclusivos de texto

### Próximos passos (após MISSÃO 4)
1. Com jogo estável, abrir diálogo real
2. Usar botão CAP para capturar
3. Analisar `[font-state]` (agora com especificadores corretos)
4. Verificar `font_frame.png`
5. Se draw válido mas texto invisível: dump GLSL e analisar amostragem R8

### Não fazer
- Repetir V-flip: NÃO
- Modificar Beluga sem evidência: NÃO

---

## Arquivos Modificados (v0068)

| Arquivo | Alteração |
|---------|-----------|
| `src/runtime/nvn/vulkan/exec.c` | `build_pipeline()`: log `PIPELINE_CREATE_BEGIN`/`END` com flush |
| `apk/AndroidManifest.xml` | `versionCode` 1792572290 |

---

## Testes

### Compilação v0068
- ✅ `SMOKE_OK`
- ✅ `RUNTIME_LINK_OK`
- ✅ `APK_OK` (373 MB)
- ✅ Assinatura SHA-256 correta

### Teste no dispositivo (PENDENTE - Snapdragon 8 Gen 2)
Conforme a ordem: "Não declarar corrigido apenas com SMOKE_OK, RUNTIME_LINK_OK ou APK_OK.
O teste precisa acontecer no Snapdragon 8 Gen 2."

**Para testar:**
1. Instale v0068
2. Jogue até a transição de menu onde v0067 crashava
3. Se crashar: extraia `$LARECOMP_DATA_ROOT/pipeline_create.log` via adb
4. Envie o log — o último BEGIN sem END identifica o pipeline suspeito

---

## Preservação

- ✅ v0067 preservada como referência
- ✅ Gameplay, áudio, controles, saves: preservados
- ✅ Sem V-flip
- ✅ Sem modificações em Beluga, mensagens, ROM
- ✅ Sem alterações globais em shaders
