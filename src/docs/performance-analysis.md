# Análise de desempenho

_17/10/2026. As medições abaixo distinguem logs anteriores, teste determinístico novo e trabalho ainda não medido. Nenhum número é atribuído a uma GPU nesta auditoria._

> **Revisão de 17/10 (validação posterior):** o ini de teste desta auditoria (`build/audit-dynamic.ini`) foi escrito com CRLF, e o leitor de `larecomp.ini` só removia o CR de `mod` e `game_lang`: `resolution = dynamic` virou "Automática" e as execuções em jogo descritas abaixo **não tinham resolução dinâmica ligada** (o próprio código antigo também não registra `[dynres]` assim, nem forçado a 20 fps). Refeito com ini LF, mesma rota do Eevee 1.0.0 a 30 fps: código antigo 1,00 → 0,60; código novo entre 0,95 e 1,00 (só reage aos travamentos de carregamento); modo 60 no mundo a 60,49 fps de média. O leitor agora remove CR/LF de toda linha (HISTORICO 56). Prioridade revista: **P3**, porque a resolução dinâmica não aparece no launcher (só `resolution = dynamic` editado à mão).

## Baseline disponível

O renderer registra janelas de aproximadamente 2 s em `runtime/nvn/vulkan/device.c`: fps da janela, maior intervalo, draws, passes, uploads e tempo de compilação. `tools/perf_summary.py` as resume. Nesta auditoria, ao processar logs **preexistentes** (sem repetir a sessão, logo não comparáveis como benchmark):

| Log | Duração | FPS médio das janelas após boot | Janelas com quadro >50 ms | Draws pulados | Programas compilados |
|---|---:|---:|---:|---:|---:|
| `logs/repro-menu.log` | 52 s | 31,4 | 4/25 | 716 | 473 |
| `logs/repro-link2.log` | 96 s | 30,7 | 6/47 | 514 | 444 |

Uma execução nova, `build/audit-dynamic-world.log` (Eevee 1.0.0, save copiado, 30 fps, resolução dinâmica, janela 1280×720, áudio desligado), chegou ao mundo no present 1450: 48 s, 23 janelas de ~2 s a 30,0 fps, uma janela com quadro máximo de 64 ms, 527 draws pulados em 7 janelas de compilação, 479 programas compilados. Não há linha `[dynres]`, portanto não houve redução registrada da escala. A captura `build/window.png` mostrou a cena 3D. **O áudio não foi testado** nessa execução.

Essas sessões envolvem cenas, compilação e opções diferentes; os números **não** medem o ganho da correção. O parser dá média de janelas, não FPS médio ponderado por quadros. O maior intervalo de cada janela tampouco permite reconstruir percentis.

Foi adicionada coleta opcional por present em `runtime/nvn/vulkan/device.c` (`LARECOMP_FRAMETIME_CSV=<caminho>`), resumida por `tools/frametime_summary.py` com percentis *nearest rank*. Numa execução nova do Eevee, `build/audit-30-frametimes.csv`, filtrando presents 1320–1449 do mundo: **130 amostras, média 33,332 ms (30,00 fps), P95 34,326 ms, P99 43,453 ms, máximo 58,980 ms, um quadro >50 ms**. São intervalos de **presents do jogo**, não tempos de trabalho da GPU. CPU/GPU, RAM e VRAM seguem sem medição.

No trecho 1360–1449, após a entrada do mundo nos dois modos, com o mesmo save copiado e entrada roteirizada:

| Modo | Amostras | Média | P95 | P99 | Máximo | >50 ms |
|---|---:|---:|---:|---:|---:|---:|
| 30 fps | 90 | 33,334 ms (30,00 fps) | 33,723 ms | 34,966 ms | 34,966 ms | 0 |
| 60 experimental | 90 | 16,540 ms (60,46 fps) | 18,152 ms | 18,585 ms | 18,585 ms | 0 |

O log do modo 60 marcou `world: 60 fps` no present 1342. As cenas podem estar em tempos de jogo diferentes apesar de terem o mesmo número de presents; a tabela compara **cadência**, não qualidade de gameplay ou ganho de GPU. Não houve linha `[dynres]` em nenhuma das duas execuções. O áudio foi desligado nos testes.

## Gargalos e custos identificados no código

- **Compilação inicial de shaders/pipelines:** `runtime/nvn/vulkan/exec.c` usa workers e orçamento de espera por draw; `renderer_stats()` expõe tradução, SPIR-V, pipelines e draws aguardados. A pesquisa anterior em `docs/HISTORICO.md`, parte 8, mediu pop-in e o efeito do cache. Repetir com cache frio/quente e nas duas GPUs alvo antes de mudar o orçamento.
- **Submissão e sincronização:** `LARECOMP_VK_PROF=1` separa tempo de execução, descriptor sets, cópias de vértices e submit; o log `[hitch]` identifica quadros acima de 50 ms. Não há amostras novas desses contadores nesta auditoria.
- **Resolução dinâmica:** havia decisão incorreta em 30 Hz, que baixava a resolução sem pressão real de desempenho. O ajuste é uma **correção de qualidade e controle**, não uma otimização de FPS.
- **Dispatch e recompilação:** `runtime/dispatch.c` usa cache direto por thread para chamadas indiretas; `runtime/cpu.h` adiciona ramo de profiler/hook em cada entrada. Há custo potencial, mas não foi perfilado, então não cabe substituir por LTO/PGO ou paralelismo especulativo.

## Comparação reproduzível da correção

`tools/test_output_scale.c` chama a implementação real de `output_scale` em 180 presents sintéticos. Compilar contra `git show HEAD:runtime/nvn/output_scale.c` e contra o arquivo alterado isola a lógica de decisão:

| Cenário | Antes | Depois | Critério |
|---|---:|---:|---|
| Modo 30, 33,33 ms por present | escala 0,70, teste falha | escala 1,00, teste passa | Não reduzir a resolução com ritmo nominal. |
| Modo 30, 45 ms por present | — | escala 0,70 | Ainda reagir a carga persistente. |
| Modo 60, 16,67 ms por present | — | escala 1,00 | Preservar o comportamento nominal de 60 Hz. |
| Modo 60, 25 ms por present | — | escala 0,70 | Reduzir a escala quando não sustenta 60 Hz. |
| Transição 30→60→30, 90 presents em cada fase nominal | — | escala 1,00 | Não misturar amostras de frequências diferentes. |

Comando do teste: `clang -O2 -Iruntime tools/test_output_scale.c runtime/nvn/output_scale.c -o build/test_output_scale.exe`, seguido de `build/test_output_scale.exe <30|60> <ms>` para os quatro valores e `build/test_output_scale.exe transition`. A escala final 0,70 depende dos 180 quadros e do cooldown atual; o teste verifica a direção da decisão.

## Próxima rodada de profiling

1. Repetir a coleta `LARECOMP_FRAMETIME_CSV` no mesmo trecho de mundo, save, entrada, resolução e cache, separando boot/loading; quantificar overhead do CSV em uma sessão pareada.
2. Em sessões 30 e 60, registrar `LARECOMP_VK_PROF`, `LARECOMP_FNPROF`, estatísticas do renderer, CPU por thread, uso de RAM/VRAM e GPU timestamps. Capturar RenderDoc/Nsight apenas nos quadros lentos; captura contínua altera o ritmo.
3. Comparar cache frio/quente, SMAA ligado/desligado e 1080p/1440p/4K. Só então escolher entre otimizar shaders, descriptor sets, uploads, submits ou sincronização.
4. Rodar o mesmo cenário em Eevee e Pikachu; manter resultados de diferentes máquinas separados.

Não há comparação de FPS antes/depois da correção porque não foi executado benchmark de jogo pareado. O teste isolado comprova a **decisão de escala**; a execução nova verifica que o jogo iniciou e apresentou o mundo sem redução registrada, mas não mede ganho de tempo de GPU.
