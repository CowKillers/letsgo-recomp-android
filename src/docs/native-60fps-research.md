# Investigação de 60 fps nativos

_17/10/2026. Síntese verificada contra o código atual, com resultados experimentais anteriores de `docs/PESQUISA-60FPS.md` e `docs/HISTORICO.md`. O modo experimental **já existe**; não foi criado nem validado em gameplay nesta auditoria._

## Laço e limite de 30 fps

O mapeamento anterior do `main` 1.0.0 identificou o laço em `f_70000231a0`, a atualização em `f_700001d1c0` e o fim do quadro em `f_7000029d30` (`docs/PROGRESSO.md`). `runtime/nvn/objects.c` registra o intervalo pedido por `nvnWindowSetPresentInterval`; `nvnQueuePresentTexture` chama `fps60_present()` e `present_pace()`. O jogo pede intervalo 2 de um vsync de 60 Hz; o runtime, por padrão, espera ~33,33 ms. A opção experimental força ~16,67 ms **apenas** quando o mundo atualiza; menu, batalha e captura conservam ~33,33 ms. `LARECOMP_FPS=60`, sozinho, só muda o pacing e acelera a lógica (medição anterior em `tools/repro.sh`).

O passo de tempo não é simplesmente o VSync. A tabela `G` está em `main+0xf7eeb8` do Eevee 1.0.0, com `G[0]=16.666.667 ns` e `G[1]=33.333.334 ns` (`main+0xf7eec0`). `f_7000039600` copia o valor para o passo 1 do objeto de temporizadores; o mundo o chama a cada atualização. `f_7000663660`/`f_7000664cd0`, `f_700067be50` e outros consumidores transformam tempo em avanço de animação. O passo 0 permanece em ~33,3 ms e alguns subsistemas apenas testam se é positivo. O mapa completo de leitores e endereços está em `docs/PESQUISA-60FPS.md`, “Mapa temporal”.

O modo 60 atual (`runtime/hle/fps60.c`) intercepta o accessor só durante a atualização do mundo, oferecendo 16,67 ms ao passo 1 e restaurando `G[1]` em seguida. `FIELD_UPDATE=0x70002f8bb0` distingue mundo de outras cenas; `ENCOUNTER_SCENE=0x700028ef70` e `BATTLE_START=0x700028bb60` marcam captura/batalha. `runtime/hle/wildbattle.c` marca a batalha selvagem pelo caminho alternativo. Endereços do Pikachu 1.0.0 são traduzidos em `runtime/fnhook.c`; patches dos contadores da cena são gerados por `tools/patches.py`, nunca editados à mão em `generated/`.

## Dependências de contagem de quadros

Animações que usam o passo 1 conseguem conservar a velocidade no mundo a 60. Linhas do tempo da captura e batalha usam contadores inteiros; `tools/patches.py` modifica três stores para a pesquisa do modo 2 (um tick a cada duas atualizações). Menus/Pokédex também contam quadros: uma abertura que levava 8 quadros a 30 levava 4 no modo 60 nativo, segundo `docs/HISTORICO.md` 47. O modo atual preserva sua lógica em 30 e apresenta quadro intermediário; batalha/captura ficam a 30 sem esse quadro porque testes em jogo expuseram defeitos de interpolação (`HISTORICO.md` 44–49). Física, partículas, scripts, câmera, áudio e todos os timers de batalha **não** foram comprovados corretos a 60 Hz.

## Alternativas

| Arquitetura | Ganho possível | Dependências e risco | Parecer atual |
|---|---|---|---|
| A. Simulação e renderização a 60 Hz | Cada present recebe cena real nova; menor latência. | Exige mapear e ajustar todos os contadores e sistemas que assumem 30 ticks/s. O modo 2 é pesquisa, não prova de correção. | Boa meta de longo prazo, risco alto de regressão. |
| B. Simulação a 30 Hz, renderização a 60 | Velocidade original com pipeline real redesenhando intermediários. | Reexecução NVN precisa identidade estável por objeto, paletas, cortes e efeitos; custo de GPU e latência. O protótipo genérico anterior foi removido por fantasmas, menus errados e modelos esticados. | Viável por cena com marcadores do jogo; não há solução geral validada. |
| C. Híbrida (atual) | Mundo a 60 sem duplicar a velocidade; demais cenas preservam comportamento conhecido. | Transições e interações entre sistemas precisam de testes; qualidade de menus depende da interpolação. | Melhor relação entre fidelidade e risco **no estado atual**, ainda experimental. |

Um quadro intermediário da alternativa B é um draw real do pipeline, com estado interpolado; não é geração por fluxo óptico. Ainda assim, sem identificar o mesmo objeto em dois quadros, a imagem pode ser incorreta. O mapeamento de `f_70006237b0`/`f_7000641190` encontrou ponteiros de objeto estáveis em parte do mundo (`docs/PESQUISA-60FPS.md`), mas não cobre todas as cenas.

## Patch público e compatibilidade

O [repositório público de Fl4sh9174](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods) contém pacotes de mods para Eevee/Pikachu. A decodificação local registrada em `docs/PESQUISA-60FPS.md` encontrou, no patch **1.0.2**, escrita de `16.666.667` em `main+0xf7eec0` e branch em `main+0x5e78b8`. A primeira coincide com `G[1]` do dump **1.0.0**; a segunda **não pode ser transposta por offset**: em 1.0.0 é `blr x8`. A hipótese de que ela altera o present interval ainda depende do binário 1.0.2. Mudar `G[1]` globalmente acelera leitores de outras cenas. A versão 120 FPS escreve ~10,7 ms, que nem corresponde a 1/120 s; não há justificativa para aplicá-la aqui.

O [RT64](https://github.com/rt64/rt64/blob/main/include/rt64_extended_gbi.h) fornece comandos de grupos de matriz com IDs e opções de interpolação. Isto sustenta a direção de marcar identidade, mas não demonstra que o protocolo do N64 possa ser copiado para NVN. O código e as medições locais têm precedência sobre o comportamento alegado de mods externos.

## Protótipo e estado da validação

O protótipo de reexecução de draws de 15/10 foi removido; imagens e falhas estão em `docs/PESQUISA-60FPS.md`. O modo híbrido atual está desligado por padrão (`fps = 30`), liga com `fps = 60` ou `LARECOMP_FPS60=1`, e mantém 30 nas cenas críticas. `LARECOMP_FPS60=2` e `LARECOMP_FPS60_MENU_NATIVE=1` são sondas de pesquisa; contador de 60 presents **não** atesta gameplay correto.

Nesta auditoria, duas execuções roteirizadas do **Eevee 1.0.0** chegaram ao mundo com save copiado. No modo 60, o log mudou para `world: 60 fps` no present 1342; nos presents 1360–1449, os intervalos tiveram média 16,540 ms, P95 18,152 ms, P99 18,585 ms, sem quadro acima de 50 ms. O modo 30 teve média 33,334 ms no mesmo intervalo de presents. A captura de tela do modo 60 mostrou o mundo, mas **não foi feita comparação de posição/tempo de gameplay, animação, captura, batalha ou áudio**. Os resultados medem apenas apresentação nessa cena curta. Também falta testar Pikachu.

## Próximas etapas técnicas

1. Instrumentar contadores e relógios por cena em Eevee/Pikachu, comparar posições, animações, turnos, captura e áudio aos mesmos segundos de jogo em 30/60.
2. Demonstrar se outros threads leem `G[1]` durante o hook; substituir a escrita global transitória por modificação local segura se a corrida existir.
3. Expandir a identidade estável dos objetos que gravam command lists. Reexecutar uma cena por vez, com testes de cortes, ossos, sombras, UI, partículas e nascimento/morte de objetos.
4. Só promover batalha/captura/menus para 60 Hz nativos depois de corrigir todos os contadores relevantes e validar o gameplay, não apenas a duração visual.
