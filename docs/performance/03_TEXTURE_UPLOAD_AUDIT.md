# 03_TEXTURE_UPLOAD_AUDIT.md

## Auditoria de Uploads de Textura
**Data:** 2026-10-21
**Status:** Investigado — uploads são sintoma, não causa

---

## Métricas (log 170)

| Métrica | Valor |
|---------|-------|
| Upload máximo | 883.6 MB / janela de 2s |
| Draws na mesma janela | 22.521 |
| Upload por draw (médio) | ~40 KB |

---

## Análise

### Os uploads são redundantes?

**NÃO.** O sistema já tem proteções:

1. **Hash check** (`textures.c:upload_texture()`):
   - Calcula hash do conteúdo antes de fazer upload
   - Se hash igual ao anterior, pula o upload
   - **Fix v0053:** `dyn_stable` reseta `dynamic=false` após 120 frames sem mudança

2. **GCOPY dedup** (`exec.c`):
   - Cache de 16384 entradas (v0053, era 4096)
   - Evita cópias duplicadas do mesmo conteúdo guest

3. **Full hash periódico:**
   - A cada 120 frames, faz hash completo para garantir consistência

### Por que 883 MB então?

Com 22.521 draws em 2 segundos:
- Cada draw referencia 1-3 texturas em média
- Mesmo com dedup, o **volume** de referências é enorme
- 883 MB / 22k draws = ~40 KB por draw
- Isso é **razoável** para texturas de 64x64 a 256x256

**Conclusão:** Os uploads são proporcionais aos draws. Reduzir uploads sem reduzir draws é impossível — são a mesma causa raiz.

---

## Oportunidades (limitadas)

1. ✅ **Já implementado (v0053):**
   - Dynamic flag reset após 120 frames estáveis
   - GCOPY_TAB 4096 → 16384
   - SMAA default OFF (reduz passes, indiretamente uploads)

2. **Não recomendado:**
   - Aumentar o intervalo do full hash (risco de stale textures)
   - Desabilitar uploads (quebra renderização)

---

## Veredito

**Os uploads NÃO são o gargalo principal.** São um sintoma dos 22k draws. A otimização deve focar em:
- Reduzir draws (impossível no runtime — conteúdo do jogo)
- Reduzir overhead por draw (Turnip — bloqueado)
- Reduzir sincronização (ETAPA 4)

**Meta de redução de uploads:** Não aplicável isoladamente.
