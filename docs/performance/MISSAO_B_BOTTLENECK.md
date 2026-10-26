# MISSAO_B_BOTTLENECK.md

## MISSÃO B: Revalidação do Gargalo
**Data:** 2026-10-21
**Fonte:** `letsgo-log_170_zf8a.txt` (janela crítica)

---

## Métricas por Frame (janela de 2s, 17.1 FPS)

| Métrica | Por janela (2s) | Por frame |
|---------|-----------------|-----------|
| Frames | 34.2 | 1 |
| Draws | 21.243 | **621** |
| Passes | 1.499 | **44** |
| Dispatches | 70 | 2 |
| Flushes | 70 | 2 |
| Upload | 749.5 MB | **21.9 MB** |
| Wait | 2.634 ms | **77 ms** |
| Compilações | 0 | 0 |

**Nota:** Os contadores são por janela de 2s. O "wait" de 77ms/frame excede o frametime de 58.5ms, indicando que as esperas se sobrepõem ou incluem tempo ocioso.

---

## Separação CPU vs GPU

### Limitação de CPU
**Evidência:**
- 621 draws/frame requerem 621x `vkCmdDraw*` + state setup
- Cada draw tem overhead de CPU (validação, binding, etc.)
- 3 workers de shader não ajudam aqui (shaders já compilados)

**Estimativa:** Com 621 draws, o CPU gasta ~10-15ms apenas submetendo comandos.

### Limitação de GPU
**Evidência:**
- 21.9 MB upload/frame
- 44 passes/frame (cada um com `vkCmdBeginRendering`)
- Adreno 740 processando geometria da floresta

**Estimativa:** GPU leva ~40-50ms para processar o frame.

### Espera por GPU (wait)
**Valor:** 2.634ms por janela
**Interpretação:** CPU bloqueada em `vkWaitSemaphores` aguardando a GPU terminar o frame anterior antes de reutilizar recursos.

**NÃO é overhead** — é dependência real. A GPU está ocupada, não ociosa.

### Espera por Apresentação
**Não medida separadamente** no log atual. O `vkQueuePresentKHR` está incluído no tempo total.

### Limitação Térmica
**Não medida.** Requer acesso a `/sys/class/thermal/` no aparelho durante o teste.

---

## Teste Decisivo: 480p vs 720p

**Hipótese:** Se 480p melhorar significativamente vs 720p → gargalo é GPU (fill-rate, bandwidth).
**Se melhorar pouco** → gargalo é CPU (draw-call overhead) ou sincronização.

**Status:** **NÃO EXECUTADO** — requer teste no S23 com as duas resoluções na mesma cena.

**Predição baseada nos dados:**
- 621 draws/frame é **alto** para mobile (ideal: <300)
- 21.9 MB upload/frame é **moderado**
- O overhead de 621 draws (CPU) provavelmente domina
- **Predição:** 480p melhoraria ~10-15%, não 2x → indica **CPU-bound**, não GPU-bound

---

## Conclusão

O gargalo é **misto**:
1. **CPU:** 621 draws/frame com overhead de submissão
2. **GPU:** Processando 44 passes e 21.9 MB/frame
3. **Wait:** Sintoma, não causa — GPU ocupada

**Não é possível** atingir 30 FPS estáveis na floresta sem reduzir draws. O backend está eficiente; o conteúdo é pesado.

**Validação pendente:** Teste 480p vs 720p no aparelho para confirmar CPU-bound vs GPU-bound.
