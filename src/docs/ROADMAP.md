# Roteiro para um port nativo maduro

_17/10/2026. Inventário do código e da documentação atual. Este arquivo é um backlog verificável, não uma afirmação de que todos os itens são bugs. O estado de cada item deve ser atualizado com links para reproduções, logs, capturas ou testes antes de uma release._

## Escopo e prioridade

O alvo principal é **Eevee e Pikachu 1.0.0 no Windows**, com gameplay fiel a 30 fps. O launcher também aceita 1.0.1/1.0.2, mas o [README](../README.md) diz que essas versões não foram testadas localmente e que as opções por endereço só valem na 1.0.0. Para anunciar suporte às atualizações é necessário testá-las; caso contrário, apresentá-las como compatibilidade não verificada. O modo 60 e as batalhas selvagens continuam **experimentais** até cumprirem critérios próprios. Recursos online, Poké Ball Plus e cooperativo precisam de decisão explícita de escopo; um produto maduro pode declarar que não os oferece, desde que não travem nem confundam o usuário.

Prioridades: **P0** perda de save, falha grave reproduzível ou corrupção ampla; **P1** bloqueia a qualidade da versão estável; **P2** melhora robustez, desempenho ou fidelidade depois dos gates; **P3** expansão opcional. Não há P0 confirmado neste inventário. Se surgir, interrompe a fila.

Cada item traz o tipo: **observado** (já reproduzido), **validação** (cobertura ausente), **hipótese** (código sugere risco, ainda sem falha medida) ou **decisão** (escopo de produto).

## Fase 1 — Provar o jogo completo e proteger os dados

Esta fase é o caminho crítico. Antes de aumentar qualidade ou FPS, precisamos saber onde a versão estável falha.

| ID | Pri. / tipo | Trabalho | Critério de conclusão |
|---|---|---|---|
| J01 | P1 / validação | Fazer uma campanha completa do **Eevee 1.0.0**, do jogo novo aos créditos, com saves em pontos fixos. Incluir ginásios, cavernas, cidades, eventos, batalhas, capturas, evolução, box e retorno a áreas anteriores. O README só registra testes até Viridian City. | Roteiro por capítulo com build, hardware, save de teste, falhas e resultado; nenhuma progressão bloqueada ou crash reproduzível. |
| J02 | P1 / validação | Repetir uma campanha própria do **Pikachu 1.0.0**, além da tela de nome; testar brincar com Pikachu e exclusivos. Não usar só save copiado do Eevee para validar paridade. | Mesmo roteiro e critérios de J01, incluindo diferenças específicas. |
| J03 | P1 / validação | Exercitar save/load em checkpoints, fechamento normal, reinício, múltiplos saves e erro de escrita/sem espaço. Auditar `runtime/services/fs.c` e a política de substituição/backup antes de mudar qualquer formato. | Save original preservado; cópia de teste abre após cada caso; falhas de escrita informadas sem perder um save válido. |
| J04 | P1 / validação | Construir matriz de regressão para boot, menus, criação, mundo, NPCs, transições, batalha, captura, save, vídeo e áudio, em **ambos** os jogos. Usar `tools/repro.sh` com diretórios de teste isolados e completar com checklist manual. | Toda correção relevante roda os cenários afetados; logs e imagens associados à build; nenhum teste toca o save do jogador. |
| J05 | P1 / validação | Testar as atualizações **1.0.1/1.0.2** dos dois jogos, ou reduzir a promessa pública de suporte até haver evidência. Verificar launcher, gerador, HLE, saves e recursos sem endereços fixos. | Tabela por título/build com funcionalidades testadas e desativadas; README e launcher concordam. |
| J06 | P1 / validação | Ensaiar release zip em Windows limpo, sem MSYS2/Python/Zig no PATH: extração, escolha dos arquivos próprios, geração, fechamento/reabertura, erros e atualização de versão. Repetir para Eevee/Pikachu. | Pacote reproduzível, sem arquivos do jogo/chaves, e roteiro de instalação completado nos dois títulos. |
| J07 | P1 / validação | Testar Windows 10/11 e GPUs NVIDIA, AMD e Intel (dedicadas e integradas compatíveis), incluindo drivers distintos, troca de foco, resize, fullscreen, suspend/resume e device lost. | Matriz publicada com versão do driver, resolução, modo, logs de validação e limitações. Não anunciar uma combinação sem teste. |
| J08 | P1 / validação | Testar sessão longa com áudio ligado, vídeo de abertura, mudança de área, batalha, captura e save/load repetidos. Medir RAM/VRAM, handles, objetos Vulkan e underruns. | Sem crescimento contínuo inexplicado, sem dessintonia perceptível e sem queda reproduzível; medições e duração registradas. |

## Fase 2 — Corrigir defeitos visíveis e endurecer o runtime

| ID | Pri. / tipo | Trabalho | Critério de conclusão |
|---|---|---|---|
| R01 | P1 / observado | Resolver a **aura dos Pokémon especiais que some por um quadro**, inclusive na release a 30 fps (`docs/PROGRESSO.md`). Usar `LARECOMP_COUNT_TEX`, `LARECOMP_COUNT_FS`, `LARECOMP_TRACE_COUNT` e um save com Pokémon de aura; evitar usar F9 como prova, pois altera o primeiro quadro gravado. | Causa localizada em draw/estado/efeito, correção mínima e comparação quadro a quadro em Eevee/Pikachu. |
| R02 | P1 / observado | Reduzir objetos que aparecem atrasados na primeira visita por shader/pipeline novo. Separar tradução, SPIR-V, criação de pipeline e espera do draw; investigar preparação antecipada por área e persistência do cache. | Mesmo percurso com cache frio/quente, contagem de draws pulados e P95/P99 antes/depois, sem regressão de loading ou RAM. Cache de pipeline é prática recomendada pela [documentação Khronos](https://docs.vulkan.org/guide/latest/pipeline_cache.html), mas o mecanismo atual precisa ser medido no jogo. |
| R03 | P1 / hipótese | Inventariar `UNIMPLEMENTED` e respostas de sucesso sem objeto em `runtime/dispatch.c`/`ipc.c`. Verificar no SDK qual `nn::Result` e payload cada chamada realmente espera; preservar stubs offline que evitam loops/crashes. | Zero sucesso falso em caminhos exercitados; erro/objeto correto demonstrado por chamada e por teste do cenário. |
| R04 | P1 / validação | Rodar Vulkan validation, synchronization validation e capturas RenderDoc em cenas representativas; investigar o caso conhecido de textura RGBA8 amostrada como sombra (`docs/HISTORICO.md`). | Nenhum erro atribuível ao runtime nos caminhos suportados, ou exceção específica documentada com impacto medido. A [Khronos](https://docs.vulkan.org/guide/latest/development_tools.html) descreve essas camadas como ferramentas para conflitos de acesso e erros de shader. |
| R05 | P1 / validação | Verificar 3D fora de Pallet Town: modelos com esqueleto em batalha, Pokémon grandes, várias cidades, cavernas, sombras, partículas e efeitos. | Conjunto de capturas de referência em 720p/1080p/1440p/4K e inspeção de geometria, depth, viewport, scissor e UI nos dois jogos. |
| R06 | P2 / hipótese | Medir leitores concorrentes de `G[1]` enquanto `accessor_hook` o altera temporariamente (`runtime/hle/fps60.c`). Se houver corrida, mover a alteração para o objeto de temporizadores ou outro ponto local validado. | Lista de leitores/threads e teste de concorrência; sem exposição de valor temporário a cenas/threads não pretendidas. |
| R07 | P2 / hipótese | Auditar crescimento de `Prog`/`Pipe`, pools de descriptors, texturas, command buffers e cache de vista em sessões longas. `exec.c` retém certos objetos para preservar identidade de ponteiros. | Contadores de objetos e bytes estabilizam em rota repetida; qualquer liberação nova passa por teste de lifetime com validation. |
| R08 | P2 / validação | Conferir semântica do código recompilado: branches indiretos/jump tables, registradores, exclusivas, barreiras, FPCR/FPSR, arredondamento, NaN, NEON/AVX2, alinhamento e stores. Priorizar instruções de funções quentes ou associadas a bugs reais. | Corpus diferencial com resultados de referência ARM64 legítimos e rastros por função; nenhum ajuste só por microbenchmark isolado. |
| R09 | P2 / validação | Confirmar códigos de retorno, timeout e cancelamento dos serviços de kernel, filesystem, input, rede offline, áudio e vídeo. O histórico mostra vários casos em que a semântica do SDK diferiu do LASRecomp. | Casos reproduzidos contra SDK/disassembly, testes de timeout e falha, sem deadlocks em reabertura/reboot. |
| R10 | P2 / decisão | Tratar ações offline sem saída útil (ex.: “Connecting...” em Presente Misterioso): saída pelo B, mensagem clara ou indisponibilidade no launcher, sem fingir serviço online. | Nenhum fluxo suportado fica preso; documentação e interface refletem o comportamento real. |

## Fase 3 — Desempenho, imagem e 60 fps com medições

| ID | Pri. / tipo | Trabalho | Critério de conclusão |
|---|---|---|---|
| D01 | P1 / validação | Definir hardware de referência e rotas fixas por cena. Registrar frametimes por present (`LARECOMP_FRAMETIME_CSV`), CPU por thread, GPU timestamps, RAM/VRAM, shader compile, draws pulados e áudio; separar cache frio/quente e boot/gameplay. | Baselines reproduzíveis 30 fps em Eevee/Pikachu e ao menos uma GPU de cada fabricante suportado, com média/P95/P99 e configuração completa. |
| D02 | P2 / hipótese | Usar perfil de CPU para escolher funções recompiladas e custos do runtime (`LARECOMP_FNPROF`, amostrador de CPU); avaliar dispatch, cópias de uniform/vertex, mutexes, alocações e hot loops. Só então experimentar flags, LTO ou PGO. | Melhora antes/depois na mesma rota e hardware, sem divergência funcional; custo de build e compatibilidade registrados. |
| D03 | P2 / hipótese | Medir `vk_full_barrier`, submits, descriptor sets, staging, uploads, cópias framebuffer e waits em `runtime/nvn/vulkan/exec.c`. Reduzir barreiras apenas com dependências de recursos conhecidas. | GPU timestamps e sync validation antes/depois, sem artefatos; a [guia Khronos de sincronização](https://docs.vulkan.org/guide/latest/synchronization.html) alerta para erro e ociosidade por sincronização incorreta. |
| D04 | P2 / validação | Dar à resolução dinâmica tempo de GPU real no Vulkan; hoje recebe `gpu_ms=-1` e reage ao intervalo de presents, que também inclui CPU/loading. Testar transições 30↔60. | Escala estável a 30/60 quando a GPU tem folga; queda só sob pressão sustentada; sem oscilação perceptível em menus/batalhas. |
| D05 | P2 / validação | Comparar SMAA ligado/desligado, anisotropia, pós-processamento, texturas finas e resolução interna verdadeira em 1080p/1440p/4K. Não adicionar TAA sem motion vectors, histórico e teste de ghosting. | Pares de captura e custo de GPU, sem UI borrada, shimmering novo, sombra desalinhada ou aumento relevante de latência. |
| F01 | P1 para promover 60 / validação | Medir posição, movimento, animação, câmera, scripts, batalha, captura, áudio e relógios aos **mesmos segundos de gameplay** em 30 e 60. Contador de presents sozinho não basta. | Suite comparativa por cena nos dois jogos; desvio quantificado e explicado. |
| F02 | P1 para promover 60 / validação | Completar o mapa de contadores de quadro e leitores da tabela `G`/passos 0 e 1; testar menus, Pokédex, cutscenes, transições e interações entre modos. | Lista de funções e subsistemas cobertos, sem aceleração ou congelamento ao mudar de cena. |
| F03 | P2 / pesquisa | Melhorar quadros intermediários somente com identidade estável do objeto e tratamento de cortes, ossos, sombras, UI e efeitos. O protótipo genérico foi removido por fantasmas e deformações. | Comparação visual quadro a quadro e em movimento; desligamento por cena quando não há identidade confiável. |
| F04 | P2 / pesquisa | Avaliar batalha/captura a 60 Hz nativos depois de corrigir contadores e física/timers, ou manter 30 de forma explícita. | Mecânicas, duração, áudio e animação iguais aos 30 fps dentro da tolerância definida pelo teste; sem regressão em ambos os jogos. |

## Fase 4 — Produto, qualidade e manutenção

| ID | Pri. / tipo | Trabalho | Critério de conclusão |
|---|---|---|---|
| M01 | P1 / validação | Criar CI para build do **runtime sem arquivos do jogo**, testes unitários e estáticos, análise de scripts/formatos e empacotamento de artefato de teste. Manter testes que exigem dump próprio do console em executor local, fora da CI pública. | Pull request falha em erro de build/teste; nenhum conteúdo do jogo, chave, gerado ou log entra no Git. |
| M02 | P1 / validação | Definir checklist de release e relatório por versão: binário limpo, dois jogos, cenários obrigatórios, save, áudio, GPUs, validação Vulkan, desempenho, documentação e rollback. | Uma release só sai com evidência anexada e limitações publicadas; testes experimentais têm seção própria. |
| M03 | P1 / observado | Corrigir divergências entre `PROGRESSO.md`, `MODO-BATALHA.md`, README e código: há trechos antigos que dizem que o jogo voltou a 30 fps ou listam como próxima etapa o protótipo de batalha já implementado. | Cada documento descreve a build atual e aponta o histórico para versões anteriores; matriz de suporte única. |
| M04 | P2 / validação | Melhorar diagnóstico para usuário: erro de setup/versão/driver, causa do crash, log copiável e instruções de recuperação sem precisar do ambiente de desenvolvimento. | Relatórios de problema contêm build, GPU/driver, título/versão, configuração e último erro sem incluir saves/chaves. |
| M05 | P2 / validação | Revisar dependências fixadas, hashes e licenças do pacote (`make dist`), além da reprodução do zip em máquina limpa. | Inventário de componentes e licenças; pacote reproduzível e sem dependência inesperada de ferramentas locais. |
| M06 | P2 / decisão | Estabelecer política de compatibilidade: Windows como plataforma suportada; Linux/Steam Deck só após build **e** teste real. Diferenciar recurso ausente, conhecido defeito e experimental no launcher/README. | Expectativa pública coincide com a matriz J05/J07, sem promessa baseada apenas em compilação. |

## Trilha opcional — recursos novos

Esses itens não devem atrasar a estabilização da experiência original a 30 fps. Se forem oferecidos como opções oficiais, passam pelos mesmos gates de regressão e paridade.

| ID | Pri. / tipo | Trabalho | Critério de conclusão |
|---|---|---|---|
| O01 | P2 / observado | Completar batalhas selvagens experimentais: XP dividido, natureza/shiny/forma, fuga, desaparecimento do Pokémon no mapa, tipos de encontro e mais áreas (`docs/MODO-BATALHA.md`). | Casos por espécie/forma/resultado, sem save corrompido e com Eevee/Pikachu equivalentes. |
| O02 | P2 / validação | Integrar e testar giroscópio real de controles compatíveis, sentido dos eixos, calibragem e deriva; manter stick/mouse como fallback. | Controle físico testado em captura e em sessões longas; sem deriva relevante. |
| O03 | P3 / decisão | Avaliar cooperativo local, Poké Ball Plus e integração online apenas se houver escopo, hardware e protocolo de teste. | Projeto decide implementar com testes completos ou declarar não suportado com saída segura. |
| O04 | P3 / decisão | Avaliar Linux/Steam Deck como port adicional, considerando Vulkan, áudio, input, paths e distribuição. | Testes de jogo real, desempenho e instalação na plataforma; não basta compilar o caminho herdado. |

## Ordem de execução sugerida

1. **J01–J04, J06 e M03:** criar cobertura de gameplay, save e release; corrigir qualquer P0/P1 encontrado. Rodar R01 em paralelo ao roteiro porque já existe reprodução visual.
2. **J05, J07–J08, R02–R05 e M01–M02:** ampliar compatibilidade e remover defeitos de apresentação/primeiro uso, com gates automáticos e manuais.
3. **D01 antes de D02–D05:** obter baselines por hardware/cena. Otimização sem perfil e sem comparação não entra como melhoria concluída.
4. **F01–F04:** promover 60 fps por subsistema, sem mudar o padrão de 30 até validar velocidade e fidelidade.
5. **O01–O04:** novas funcionalidades conforme decisão de produto e capacidade de teste.

## Definição de “versão estável madura”

- Campanha principal concluída em Eevee e Pikachu nas builds anunciadas, com saves íntegros e sem bloqueio reproduzível.
- Cenários críticos J04 passam em release limpa; nenhum P0/P1 conhecido sem correção ou limitação pública específica.
- Renderização e sincronização Vulkan verificadas nas GPUs anunciadas; aura e pop-in de primeira visita resolvidos ou classificados com impacto e plano explícitos.
- Baselines de frametime, áudio e memória publicadas para hardware de referência; desempenho medido em gameplay, não inferido de um contador de FPS.
- Release reproduzível sem conteúdo do jogo ou chaves, com instalação e diagnóstico que funcionam fora da máquina de desenvolvimento.
- 60 fps e batalhas selvagens só deixam o rótulo “experimental” quando sua própria matriz de gameplay, temporização e paridade estiver completa.
