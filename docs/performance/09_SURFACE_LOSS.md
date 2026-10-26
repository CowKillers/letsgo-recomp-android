# 09_SURFACE_LOSS.md

## ETAPA 9: Perda de Superfície Vulkan
**Data:** 2026-10-21
**Status:** Mecanismo existe, mas precisa de guarda para background

---

## Problema

Log mostra `VK_ERROR_SURFACE_LOST_KHR` ao minimizar o app. A função `recover_lost_surface()` existe mas pode falhar se:
1. `ANativeWindow` for nula (app em background)
2. Tentar recriar superfície sem janela válida

## Solução Implementada

Adicionar verificação: se a janela não está disponível (app em background), suspender a apresentação em vez de tentar recriar.

**Arquivo:** `src/runtime/nvn/vulkan/device.c`

**Mudança:** Em `recover_lost_surface()`, antes de tentar recriar:
```c
// Se o app está em background (sem janela válida), não tentar recriar
// Apenas marcar como suspenso e aguardar onResume
if (!window || !is_window_available()) {
    LOG("[vk] surface lost while in background: suspending presentation\n");
    g_vk.surface_suspended = true;
    return;
}
```

**Nota:** Requer implementação de `is_window_available()` via JNI ou flag do ciclo de vida Android.

---

## Status

**Documentado, não implementado** — requer teste em aparelho para validar o ciclo onPause/onResume. Sem aparelho conectado, não é seguro modificar o ciclo de vida.

**Recomendação:** Implementar quando houver acesso ao S23 para testar minimizar/retomar.
