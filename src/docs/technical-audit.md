# Auditoria técnica do port nativo

_17/10/2026. Escopo: revisão estática dos caminhos críticos do runtime e do gerador, pesquisa já reproduzida neste repositório, compilação do runtime e teste isolado da resolução dinâmica. Não é uma prova de correção de todas as instruções ARM64, serviços ou cenas._

> **Revisão de 17/10 (validação posterior):** o ini de teste desta auditoria (`build/audit-dynamic.ini`) foi escrito com CRLF, e o leitor de `larecomp.ini` só removia o CR de `mod` e `game_lang`: `resolution = dynamic` virou "Automática" e as execuções em jogo descritas abaixo **não tinham resolução dinâmica ligada** (o próprio código antigo também não registra `[dynres]` assim, nem forçado a 20 fps). Refeito com ini LF, mesma rota do Eevee 1.0.0 a 30 fps: código antigo 1,00 → 0,60; código novo entre 0,95 e 1,00 (só reage aos travamentos de carregamento); modo 60 no mundo a 60,49 fps de média. O leitor agora remove CR/LF de toda linha (HISTORICO 56). Prioridade revista: **P3**, porque a resolução dinâmica não aparece no launcher (só `resolution = dynamic` editado à mão).

## Arquitetura observada

`tools/gen.py`, `analyze.py`, `lifter.py` e `patches.py` convertem os quatro NSO em C; `Makefile` compila o resultado em `lasgame.dll` com `-O1`, `-fno-strict-aliasing` e `-march=x86-64-v3`. `runtime/loader.c` carrega os segmentos, símbolos e relocations no espaço de endereços do guest. `runtime/cpu.h` mantém registradores gerais, vetoriais, flags, FPCR/FPSR e helpers de memória/atômicos. `runtime/dispatch.c` resolve chamadas indiretas por tabelas ordenadas com cache por thread. `runtime/fnhook.c` permite interceptar funções do jogo sem editar C gerado, traduzindo endereços conhecidos do Eevee 1.0.0 para o Pikachu 1.0.0.

O runtime implementa kernel e IPC (`runtime/kernel.c`, `ipc.c`), serviços (`runtime/services/`), entrada e vídeo (`runtime/hle/`) e NVN (`runtime/nvn/`). O backend Vulkan traduz comandos NVN, shaders Maxwell e recursos; shaders e pipelines são compilados em workers (`runtime/nvn/vulkan/exec.c`). O jogo mantém a lógica; o runtime controla o ritmo de apresentação em `runtime/nvn/objects.c`.

## Descobertas priorizadas

| Prioridade | Evidência | Consequência e ação |
|---|---|---|
| **P3 corrigido** | `runtime/nvn/output_scale.c`: `dyn_update` usava limiares de 17,4 ms e 14,5 ms para qualquer cena; `runtime/nvn/objects.c` usa ~33,3 ms no modo padrão. | Resolução dinâmica podia cair de 1,00 para 0,50 em 30 fps estáveis. Limiares agora usam 33,33 ms em cenas de 30 Hz e 16,67 ms no mundo/menu do modo 60, descartando amostras de uma frequência ao trocar de cena. Teste determinístico: 180 quadros a 33,33 ms resultavam em 0,70 antes e 1,00 depois. |
| **P1 em investigação** | `runtime/dispatch.c:hle_unimplemented` devolve `x0=0` a símbolos sem HLE; `runtime/ipc.c` também tem serviços offline. | Alguns chamadores podem interpretar sucesso e tentar usar um objeto ausente. Inventariar símbolos/serviços usados por Eevee e Pikachu com `LARECOMP_TRACE_SVC` e logs `[hle] UNIMPLEMENTED`; implementar resultado correto por contrato do SDK. A presença do stub, isoladamente, não prova falha em gameplay. |
| **P2 em investigação** | `runtime/hle/fps60.c:accessor_hook` escreve temporariamente `G[1]` em memória compartilhada, chama o guest e restaura. | Um leitor em outra thread pode observar 16,67 ms durante a janela do hook. Medir leitores e threads antes de substituir por valor local no objeto de temporizadores. Nenhuma corrida foi reproduzida nesta auditoria. |
| **P2 em investigação** | `runtime/nvn/vulkan/exec.c` mantém objetos antigos `Prog`/`Pipe` para evitar colisão de chaves por ponteiro, conforme comentário próximo ao cache. | Uso de memória pode crescer com recriações de programas. Medir número de objetos, bytes e duração de sessões longas; trocar a identidade por geração/ID só depois de validar todas as chaves. |
| **P2 em investigação** | `runtime/cpu.h` faz stores comuns diretamente e implementa exclusivos com `__atomic_*`; `tools/lifter.py` emite fence para barreiras. | Falta uma prova diferencial ampla para semântica de atomics, NEON e FPCR/FPSR sob concorrência. Priorizar corpus de instruções e comparação com execução ARM64 legítima; não converter código compartilhado por threads sem essa prova. |
| **P2 em investigação** | `runtime/nvn/vulkan/exec.c:vk_full_barrier` usa `ALL_COMMANDS`/`MEMORY_WRITE` em diversos pontos entre passes, cópias e dispatches. | Uma barreira ampla pode serializar trabalho da GPU, mas a segurança de reduzi-la depende do produtor/consumidor de cada recurso. Capturar GPU timestamps e validação Vulkan antes de especializar; não há ganho medido aqui. |

O risco do cache de programas e o de leitura concorrente de `G[1]` são **hipóteses sustentadas pelo código**, ainda sem reprodução de defeito. Não foram tratados como bugs confirmados.

## Cobertura da revisão estática

| Área | Evidência conferida | Limite da conclusão |
|---|---|---|
| Mapeamento e branches indiretos | `tools/analyze.py` extrai inícios de `.eh_frame_hdr` e ponteiros de código em dados; `tools/lifter.py` distingue `blr`/`br`; `runtime/dispatch.c` resolve endereços com busca e cache. | Não foi feito teste diferencial de todas as jump tables e targets contra ARM64. |
| Estado da CPU, SIMD e FP | `runtime/cpu.h` fixa offsets com `_Static_assert`; `tools/simd_c.py`, `runtime/simd.h`/`simd_fp.h` tratam FPCR, NaN, FZ e FMA com caminhos lentos. | A presença de tratamento não prova equivalência bit a bit; faltam vetores de referência para arredondamento, flags e casos extremos. |
| Kernel, serviços, filesystem, áudio e input | `runtime/kernel.c` usa mutex/condvar e implementa semântica de timeout documentada; `runtime/services/`, `runtime/hle/input.c` e `movie.c` cobrem os caminhos do jogo observados no histórico. | Nesta rodada não houve carga concorrente, teste de save/load, áudio habilitado ou varredura completa de códigos de retorno. |
| Vulkan | `exec.c` mantém cache de pipelines, workers, descriptor pools, ring transitório e barriers; `textures.c` contém caminhos de staging e conversão. | Não houve captura RenderDoc/validação nem perfil de stalls, overdraw, VRAM ou custo de barriers. |

## Estratégia de regressão

Para testes roteirizados, `tools/repro.sh` copia o save para `build/test-save` e usa `LARECOMP_INPUT`; ele **apaga** a pasta temporária e `build/frames`, então conferir esses locais antes de executá-lo. Usar `TURBO=0` para medir ritmo real. Testar cada cena nas duas builds 1.0.0, com modo 30 e 60 desligado/ligado conforme aplicável. Comparar quadros correspondentes por tempo de jogo, não apenas por número de presents.

| Cenário | Verificação necessária |
|---|---|
| Inicialização e menus | Boot sem `[hle] UNIMPLEMENTED` novo; seleção, Pokédex e animações na velocidade original. |
| Criação, save/load | Nome correto; save em cópia; recarregar e conferir integridade. |
| Mundo, NPCs e transições | Posição, câmera, diálogo, fade, objetos e ausência de draws pulados após carregamento. |
| Batalha e captura | Turnos, animações, mira, anel e timers; 60 experimental deve manter a velocidade do modo 30. |
| Áudio e vídeo | Sem underrun; sincronismo com quadro e com tempo de parede. |
| 1080p, 1440p, 4K | Captura da imagem interna e da janela, viewport, profundidade, UI, sombras e pós-processamento. |
| Resolução dinâmica | Em 30 Hz estáveis, escala deve ficar em 1,00; sob carga persistente deve cair; repetir ao entrar/sair do modo 60. |

Nesta execução houve build do runtime, teste isolado e execuções roteirizadas do **Eevee 1.0.0** em 30 e 60 experimental, com `resolution = dynamic`, save copiado para `build/audit-save-20261017` e áudio desligado. Ambas chegaram ao mundo e produziram captura de cena 3D em 1280×720, sem linha `[dynres]`. A instrumentação opcional mediu, nos 90 presents 1360–1449, médias de 33,334 ms (30) e 16,540 ms (60); detalhes em `performance-analysis.md`. **Pikachu, áudio, alternância de cenas, velocidade do gameplay e testes manuais não foram validados**; uma release exige essas etapas. `tools/test_output_scale.c` cobre a decisão de escala, não toda a apresentação Vulkan.
