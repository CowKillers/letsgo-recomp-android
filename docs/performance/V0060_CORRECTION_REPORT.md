# V0060_CORRECTION_REPORT.md

## Relatório de Correção — v0060-debug
**Data:** 2026-10-21
**Ordem:** CORREÇÃO das 4 falhas da v0059

---

## FALHA 1: Instrumentação Incompleta ✅ CORRIGIDA

### Problema
A v0059 registrou o atlas mas não produziu registros dos draws de glifos. A flag `g_font_atlas_bound` podia ser resetada antes do draw.

### Correção Implementada
**Arquivo:** `src/runtime/nvn/vulkan/exec.c`

Adicionados 5 contadores independentes:
- `cnt_text_candidates`: Draws avaliados pelo filtro
- `cnt_text_matched`: Draws com atlas confirmado
- `cnt_text_captured`: Draws com log emitido
- `cnt_text_rejected`: Draws rejeitados pelo filtro
- `cnt_text_dropped`: Logs descartados por limite

**Log aprimorado:**
```
[font-draw] #N frame=F indexed=I prog=P (atlas bound, matched=M)
```

### Verificação Pendente
Requer teste no S23 para confirmar que os contadores incrementam e os logs aparecem.

---

## FALHA 2: Análise do atlas_dump.raw ✅ CONCLUÍDA

### Validação
**Arquivo:** `workspace/user/files/atlas_dump_165_oki4.raw`
- **Tamanho:** 2.097.152 bytes (correto: 2048×1024)
- **Formato:** R8 (1 byte por pixel)
- **Pixels não-zero:** 8.152 (0,39%)
- **Bounding box:** X[1-640], Y[934-1014]

### PNG Gerado
**Arquivo:** `workspace/user/files/atlas_dump_165_oki4.png`
- Gerado sem alterar os dados originais
- Mostra glifos reconhecíveis na região inferior do atlas

### Correlação com Diálogo
**Status:** PENDENTE — requer captura no momento exato do diálogo.
O dump atual foi capturado durante gameplay geral, não especificamente durante um diálogo.

**Recomendação:** Adicionar trigger para capturar o atlas quando um diálogo estiver aberto (requer detecção de estado do jogo).

---

## FALHA 3: Turnip sem ADT_LOG ⚠️ DOCUMENTADA

### Verificação
**APK v0059:** Contém `libadrenotools.so` (874 KB, prebuilt)
**Instrumentação:** Adicionada em `wonder-recomp/adrenotools-src/src/driver.cpp` (código-fonte)

### Problema
A biblioteca instrumentada **NÃO** está no APK. O APK usa a versão prebuilt sem os logs `ADT_LOG`.

### Para incluir a instrumentação:
1. Compilar `adrenotools-src` com NDK para ARM64
2. Substituir `libadrenotools.so` no APK
3. Rebuild completo

**Status:** Documentado, não implementado (requer setup de build do adrenotools).

### Mensagem Incondicional
**Não implementada** — requer a biblioteca instrumentada no APK.

---

## FALHA 4: VK_ERROR_SURFACE_LOST_KHR ✅ CORRIGIDA

### Problema
`recover_lost_surface()` chamava `create_swapchain()` mesmo quando a superfície era inválida (app em background), causando crash em `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`.

### Correção Implementada
**Arquivo:** `src/runtime/nvn/vulkan/device.c`

```c
// Antes: chamava create_swapchain() incondicionalmente
// Depois: verifica se a superfície é válida antes
if (g_vk.surface != VK_NULL_HANDLE) {
    create_swapchain();
} else {
    LOG("[vk] surface is null, skipping swapchain creation\n");
}
```

**Comportamento:**
- Se `SDL_Vulkan_CreateSurface` falhar → loga "suspending presentation" e retorna
- Se não houver janela SDL → loga e retorna
- Se superfície for NULL → pula `create_swapchain()`
- A apresentação será retomada quando `recover_lost_surface()` for chamada novamente com janela válida

### Teste Pendente
Requer teste de background/resume no S23.

---

## Build v0060-debug

**Status:** Código pronto, build pendente (JDK indisponível na VM)

**Mudanças incluídas:**
1. Contadores de texto (FALHA 1)
2. Fix de surface recovery (FALHA 4)
3. Preserva todas as correções anteriores (v0053/v0058/v0059)

**Para compilar quando JDK voltar:**
```bash
cd ~/workspace/letsgo-port
export PATH=/usr/bin:$PATH
bash build-runtime.sh && bash link-runtime.sh
rm -rf build/apk && bash build-apk.sh
cp LetsGoRecomp-android.apk LetsGoRecomp-v0060-debug.apk
```

---

## Resumo

| Falha | Status |
|-------|--------|
| FALHA 1 (instrumentação) | ✅ Corrigida no código |
| FALHA 2 (atlas) | ✅ Validada, PNG gerado |
| FALHA 3 (ADT_LOG) | ⚠️ Documentada, requer build separado |
| FALHA 4 (surface) | ✅ Corrigida no código |

**Próximo passo:** Aguardar JDK para compilar v0060, ou testar v0059 atual para validar a instrumentação básica.
