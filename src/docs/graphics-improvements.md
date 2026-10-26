# Auditoria gráfica

_17/10/2026. Revisão de código e das capturas/medições já documentadas em `docs/HISTORICO.md`; sem nova captura RenderDoc nesta rodada._

> **Revisão de 17/10 (validação posterior):** o ini de teste desta auditoria (`build/audit-dynamic.ini`) foi escrito com CRLF, e o leitor de `larecomp.ini` só removia o CR de `mod` e `game_lang`: `resolution = dynamic` virou "Automática" e as execuções em jogo descritas abaixo **não tinham resolução dinâmica ligada** (o próprio código antigo também não registra `[dynres]` assim, nem forçado a 20 fps). Refeito com ini LF, mesma rota do Eevee 1.0.0 a 30 fps: código antigo 1,00 → 0,60; código novo entre 0,95 e 1,00 (só reage aos travamentos de carregamento); modo 60 no mundo a 60,49 fps de média. O leitor agora remove CR/LF de toda linha (HISTORICO 56). Prioridade revista: **P3**, porque a resolução dinâmica não aparece no launcher (só `resolution = dynamic` editado à mão).

## Pipeline atual

Os comandos NVN são traduzidos em `runtime/nvn/cmdbuf.c` e executados em `runtime/nvn/vulkan/exec.c`. `runtime/nvn/vulkan/textures.c` dimensiona render targets escaláveis e mantém texturas preenchidas pela CPU no tamanho original. `runtime/nvn/output_scale.c` calcula escala interna separada do retângulo de apresentação. O backend aplica viewport e render targets na escala calculada; a apresentação ocupa o retângulo da janela. Assim, o caminho implementa **renderização interna ampliada**, além de ampliar a imagem final. A evidência anterior de passes e dimensões está em `docs/HISTORICO.md`, parte 8; a correção visual em 4K ainda requer capturas atuais.

`runtime/nvn/vulkan/exec.c:post_view` aplica SMAA 1x na imagem do jogo em três passes (bordas, pesos, mistura). FSR 1 EASU/RCAS é usado apenas quando a imagem precisa ser ampliada até a janela. A apresentação com redução de resolução usa múltiplas amostras; texturas com mipmap e filtro linear podem receber anisotropia 16× (`textures.c`). O pipeline cache Vulkan é persistido periodicamente (`device.c`, `exec.c`).

## Problema corrigido

**P3 — Resolução dinâmica baixava a escala no ritmo nominal de 30 fps.** O comparador de 17,4 ms em `output_scale.c` refletia 60 Hz, mas o jogo normalmente apresenta a cada ~33,3 ms (`runtime/nvn/objects.c`). Em 180 amostras sintéticas, a escala ia de 1,00 a 0,70. Agora o orçamento acompanha 30/60 Hz, e uma mudança de cena descarta a janela de amostras anterior. O teste isolado mediu escala 1,00 a 33,33 ms e 0,70 a 45 ms. Uma execução roteirizada do Eevee mostrou o mundo em 1280×720 após 1450 presents a 30 fps, sem linha `[dynres]`. Ainda faltam Pikachu, transições e comparação visual pareada com a versão anterior.

## Limitações e riscos

- A resolução dinâmica do Vulkan passa `gpu_ms = -1` em `device.c`; decide pela duração entre presents. Quedas por CPU, espera de shader ou loading podem provocar redução indevida. A heurística já ignora pausas acima de 250 ms, mas isso não separa todo hitch de GPU. **P2**, medir com timestamps de GPU antes de alterar.
- O modo 60 experimental não é uniforme: mundo a 60, menus com quadro intermediário a partir de lógica 30, batalha/captura a 30. A resolução dinâmica deve ser inspecionada em cada transição. **P2**.
- Resolução de sombra maior, TAA, LOD e precisão de render targets não foram adotados: faltam comparação visual e custo de GPU. O mapa de sombras 2048² e o risco de escalar alvos que não são 16:9 constam em `docs/HISTORICO.md`.
- SMAA afeta a imagem já composta, inclusive interface. Avaliar arestas da UI e texturas finas por pares de captura antes de trocar por FXAA/TAA. TAA depende de motion vectors e tratamento de cortes/ghosting que este pipeline não oferece hoje.

## Validação recomendada

Capturar `LARECOMP_SHOT_AT` e `LARECOMP_WINDOW_SHOT` em 1080p, 1440p e 4K com a mesma câmera. Conferir pixels e dimensões de RT HDR, depth, bloom e UI no `LARECOMP_DEBUG_FRAME`/RenderDoc; comparar com `LARECOMP_NO_SCALE=1`. Inspecionar viewport/scissor, alinhamento de sombras e pós-processamento. Repetir com SMAA on/off e anisotropia 1×/16× e medir custo por GPU timestamps. Executar em Eevee e Pikachu antes de release.
