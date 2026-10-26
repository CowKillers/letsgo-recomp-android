# 04_SYNC_ANALYSIS.md

## Análise de Sincronização
**Data:** 2026-10-21
**Status:** Investigado — wait é dependência real, não redundância

---

## Onde o wait acontece

**Função:** `vk_wait_seq(uint64_t seq)` em `exec.c:76-89`

```c
VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
```

**Chamadas:**
- `exec.c:98`: Espera antes de reutilizar command buffer
- `exec.c:303,310`: Espera antes de destruir recursos
- `exec.c:524`: Espera antes de destruir buffer VMA
- `exec.c:2005`: Espera no flush

---

## Classificação das Esperas

| Tipo | Descrição | Necessária? |
|------|-----------|-------------|
| Dependência obrigatória | Esperar GPU terminar antes de reutilizar command buffer | **SIM** |
| Espera por GPU ocupada | 2.9s na floresta — GPU processando 22k draws | **SIM** (GPU lenta) |
| Espera por apresentação | `vkQueuePresentKHR` | **SIM** |
| Sincronização redundante | — | **NENHUMA ENCONTRADA** |

---

## Por que 2.9s?

O `wait` de 2.944 ms NÃO é overhead de sincronização. É o tempo real que a GPU Adreno 740 leva para processar 22.521 draws.

**Cálculo:**
- 22.521 draws / 2.944s = ~7.650 draws/segundo
- Cada draw tem overhead fixo de CPU+GPU
- A GPU está saturada, não ociosa

**Eliminar o wait causaria:**
- Uso de command buffers ainda em uso pela GPU → corrupção
- Destruição de recursos em uso → crash
- Hazards de leitura/escrita → artefatos

---

## Otimização do LASRecomp-Android-Plus (não aplicável)

A referência usa um **watcher thread com fences** em vez de timeline semaphores, porque:
- Turnip no kernel KGSL emula timeline semaphores em software (lento)
- Com fences individuais + thread dedicada, evita bloqueio

**Por que não portar:**
1. Estamos usando driver do sistema (Adreno), não Turnip
2. Timeline semaphores são nativos e eficientes no driver Qualcomm
3. O gargalo é a GPU ocupada, não o mecanismo de sincronização
4. Portar exigiria reescrever todo o sistema de submissão (alto risco)

---

## Veredito

**Nenhuma sincronização desnecessária encontrada.** O `wait` é proporcional à carga da GPU. A solução é reduzir a carga (menos draws), não eliminar sincronização.

**Critério de aprovação:** N/A — sem alterações seguras possíveis.
