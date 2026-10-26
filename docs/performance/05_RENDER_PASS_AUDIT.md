# 05_RENDER_PASS_AUDIT.md

## Auditoria de Render Passes
**Data:** 2026-10-21
**Status:** Investigado — passes são dirigidos pelo jogo, não otimizáveis no runtime

---

## Métricas (log 170, trecho crítico)

| Métrica | Valor |
|---------|-------|
| Passes | 1.701 |
| Draws | 18.387 |
| Draws por pass (médio) | 10.8 |

---

## Implementação

**Técnica:** Dynamic Rendering (`vkCmdBeginRendering`, Vulkan 1.3)
**Local:** `exec.c:977-984`

```c
vkCmdBeginRendering(cb, &ri);
in_pass = true;
stat_passes++;
```

**NÃO** usa RenderPass tradicional com framebuffers — usa rendering dinâmico.

---

## Por que 1.701 passes?

Cada `vkCmdBeginRendering` corresponde a uma mudança de render target solicitada pelo jogo:
- Shadow maps (depth pre-passes)
- Main color buffer
- Post-processing targets
- UI overlays

Com 18k draws distribuídos em 1.7k passes, a média de 10.8 draws/pass indica:
- Muitos passes pequenos (shadow maps, etc.)
- Comportamento normal do jogo, não ineficiência do runtime

---

## Agrupamento possível?

**NÃO.** Os passes são iniciados quando o jogo muda:
- `st.rt.color[]` (render targets)
- `st.rt.depth` (depth buffer)
- Viewport/scissor

Agrupar passes com targets diferentes violaria dependências de leitura/escrita e quebraria a renderização. O jogo controla isso via comandos NVN; o runtime apenas traduz.

---

## Veredito

**Passes NÃO são otimizáveis no runtime.** São 1:1 com as solicitações do jogo. Reduzir passes exigiria modificar o comportamento do jogo (impossível sem hacks específicos por cena — proibido pela Regra 8).

**Critério de aprovação:** N/A — sem alterações seguras possíveis.
