# 07_08_RESOLUTION_FRAMING.md

## ETAPA 7: Resolução e FSR1 ✅
**Status:** Verificado e funcionando

### Mapeamento (v0051, ordem da Emily)
| Seleção | Resolução | LARECOMP_SCALE |
|---------|-----------|----------------|
| 0.5x | 480p | 0.4444 |
| 0.75x | 540p | 0.5 |
| 1x | 720p | 0.6667 |
| 1.5x | 1080p | 1.0 |

### FSR1
- **Pipeline:** 1280x720 → FSR1 → 1920x1080
- **Status:** Funcionando (log confirma `1280x720 fsr1 -> 1920x1080`)
- **Transformação:** `window 2340x1080 -> swapchain 1080x2340` (rotação correta, NÃO alterar)

### Conclusão
Resolução e FSR1 estão corretos. Reduzir resolução ajuda fill-rate, mas NÃO resolve os 22k draws (overhead de CPU/GPU por draw, não por pixel).

---

## ETAPA 8: Frame Pacing ✅
**Status:** Configurado para 30 FPS

### Configuração
- **Alvo:** 30 FPS (33.33ms)
- **Present mode:** FIFO (VSync)
- **Frames in flight:** Padrão do runtime

### Observação
O jogo atinge 30 FPS em cenas leves e 17-18 FPS na floresta. O frame pacing está correto; a queda é por sobrecarga de draws, não por pacing incorreto.

**Não alterar** a velocidade da simulação (Regra 7).
