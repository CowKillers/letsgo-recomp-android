# Pesquisa: como outros ports chegaram a 60 fps

_15/10/2026. **O protótipo descrito abaixo foi removido do runtime no mesmo dia** (erros visuais demais: ver o fim
da seção do protótipo); o jogo fica nos 30 fps originais. Esta página fica como registro. Contexto: o Let's Go avança a lógica um passo por quadro; o passo de tempo existe, mas só parte do jogo o usa
("Mapa temporal", no fim desta página). Destravar o limite deixa o jogo em velocidade dobrada._

## As quatro técnicas que existem

| Técnica | Como funciona | Quem usa | Serve para o Let's Go? |
|---|---|---|---|
| **1. Só destravar o limite** | troca o intervalo de present de 2 para 1 | mods de 60 fps de Switch (patch `.pchtxt`/IPS no `nvnWindowSetPresentInterval`) | **Não**: a lógica dobra de velocidade. O GBAtemp confirma que os mods de 60 fps do Let's Go têm exatamente esse problema |
| **2. Motor já usa delta time** | o jogo calcula a física pelo tempo real do quadro; o port só destrava e corrige bugs que aparecem em fps alto | Sonic Unleashed Recompiled (o jogo original já era sem taxa fixa) | **Não**: medimos que o Let's Go não usa tempo do quadro |
| **3. Interpolação de transformações** (a mais usada em ports) | a lógica continua a 30 Hz; para cada quadro extra, o renderizador redesenha a cena com as **matrizes** (câmera, objetos, ossos) a meio caminho entre o quadro anterior e o atual | Ship of Harkinian (Ocarina of Time), Zelda64Recomp e outros com RT64, sm64ex | **Sim, é o caminho**, adaptado ao nosso nível (NVN em vez de display lists de N64) |
| **4. Geração de quadros por imagem** | cria o quadro do meio a partir de duas imagens prontas (fluxo óptico) | programas externos de geração de quadros, recursos de driver | Funciona sem mexer no jogo, mas a qualidade é pior (artefatos na interface e nos textos) e o atraso aumenta |

## Como a interpolação de transformações funciona nos ports

### Ship of Harkinian (código fonte decompilado de Ocarina of Time)
Arquivo `soh/soh/frame_interpolation.cpp`:
- A cada quadro de lógica, **grava uma árvore** de todas as operações de matriz (translate, rotate, multiplicar,
  push/pop), indexada pelo objeto do jogo e pela ordem das chamadas (`FrameInterpolation_StartRecord`,
  `FrameInterpolation_RecordOpenChild`).
- Para um quadro intermediário, **percorre a árvore nova e a anterior juntas** (`interpolate_branch`) e mistura os
  valores com um fator `step` (0,5 para 60 a partir de 30). Ângulos usam o caminho mais curto; diferença grande
  demais é tratada como corte (não mistura).
- Casos especiais: matrizes relativas ao ator (evitam o efeito "papel" em rotações de 180°), câmera que pode pedir
  "não interpolar" num corte de cena, objetos que aparecem/somem (desenhados sem mistura), rastros (ribbons) e
  **membros com skinning** (guarda pose anterior e atual dos vértices).
- Funciona porque eles têm o código fonte: chamam essas funções em cada lugar que monta uma matriz.

### RT64 (Zelda64Recomp, recompilação estática como a nossa)
- O renderizador roda **dois renderizadores**: um sincronizado com o jogo (que lê a memória no ritmo original) e outro
  que **reexecuta os draw calls** gravados, em resolução maior e em outra taxa, interpolando.
- Para casar o que é o mesmo objeto entre dois quadros, os patches do jogo inserem **comandos extras** no display list
  (`gEXMatrixGroup`, com um `id` e flags dizendo o que interpolar: posição, rotação, escala, vértices, textura...).
  Sem `id`, há modo automático pela ordem.
- Mudar o fps **não altera a física nem a velocidade** do jogo (relatos sobre o Zelda64Recomp).

### sm64ex (Super Mario 64)
Patch de 60 fps aplicado no código decompilado: lógica a 30 Hz, animações e objetos interpolados no caminho de
renderização.

## O que isso significaria no nosso runtime

Nós não temos o código fonte (como o SoH) nem display lists de N64 (como o RT64). Mas temos algo parecido com o RT64:
**todos os comandos NVN de cada quadro passam pelo runtime** (`runtime/nvn/cmdbuf.c`, `vulkan/exec.c`). Um protótipo:

1. **Gravar o quadro**: guardar a lista de draws do quadro N (programa, buffers, cópias dos uniform buffers).
2. **Casar draws entre N−1 e N**: pela ordem dentro do passe, programa e endereço do vertex buffer (como o modo
   automático do RT64).
3. **Quadro intermediário**: reexecutar os draws de N com os **uniform buffers misturados** entre N−1 e N:
   - `cb3` (mundo e câmera) e `cb4` (paleta de ossos) são matrizes: misturáveis;
   - cortes de câmera e objetos novos/sumidos: sem mistura (como no SoH).
4. Apresentar o intermediário entre os dois quadros reais: custa **um quadro de atraso** (~33 ms).

Riscos e incertezas:
- O casamento de draws pode falhar em cenas com muitos objetos, partículas e listas que mudam de ordem.
- Interface (menus, textos) e efeitos de tela cheia geralmente não devem ser interpolados.
- Dados calculados pela GPU (compute) e texturas animadas não se interpolam assim.
- Interpolar matrizes com perspectiva ou escala exige decompor (o RT64 tem modo `DECOMPOSE`).
- É trabalho grande e exploratório: semanas, não dias. Mas é a mesma família de técnica que os ports bem-sucedidos
  usam.

## Protótipo (15/10/2026): FPS 60 no launcher / `LARECOMP_INTERP=1`

Implementado em `runtime/nvn/vulkan/exec.c` (seção "frame interpolation") e `device.c` (`backend_present`):
- **Gravação**: as listas de comandos de cada quadro (com um hash do conteúdo), o estado de execução no início do
  quadro e uma cópia de cada uniform buffer (cbuf 3+) dos estágios de vértice e fragmento. Chave de cada cópia:
  estágio, binding, programa, vertex buffer 0, parâmetros do draw e a ocorrência dessa chave no quadro.
- **Reexecução** no present do quadro N: as mesmas listas a partir do estado inicial, pulando o que escreve na memória
  do jogo (compute, cópias para buffer e de buffer para textura, atualização inline de uniform, syncs, contadores).
  Cada uniform é a mistura palavra a palavra das cópias de N−1 e N, só para floats normais e sem salto grande (corte de
  câmera, texturas que rolam); o resto, e draws sem par no quadro anterior, ficam com o valor de N.
- **Apresentação**: guarda a imagem de N numa cópia, mostra o intermediário e, meio período depois, N.
- **Segurança**: sem intermediário quando alguma lista mudou desde a execução (o jogo reaproveitou a memória) ou
  quando o jogo destruiu texturas, pools ou programas no quadro (o estado gravado poderia apontar para eles).

Resultados (Pallet Town, `tools/repro.sh` andando com o analógico):
- **Funciona**: entre os quadros 1329 e 1330 o cenário anda 12 px; o intermediário está a **6 px** de cada um (medido
  por alinhamento das imagens). Personagem e Eevee também ficam no meio do caminho; menus aparecem corretos.
- **Estável**: 2.000 quadros sem erro, sem avisos da validação do Vulkan; intermediário em ~99% dos quadros; a lógica
  continua a 30 fps (o custo de GPU dobra: cada quadro é desenhado duas vezes).
- **Defeitos conhecidos**: uma linha branca fina perto de uma porta no intermediário (provavelmente duas peças de
  geometria, uma com par e outra sem); atraso de meio quadro (~17 ms) a mais.
- **Não testado**: batalhas, cenas, efeitos com partículas e compute, sombras de objetos que surgem/somem.

Para testar: escolher 60 fps no launcher (`larecomp.ini`: `fps = 60`), ou `LARECOMP_INTERP=1` + `LARECOMP_WINDOW_SHOT=<frame>` (grava `build/window_mid.png`,
o intermediário, e `build/window.png`, o quadro). O `LARECOMP_SHOT_AT` grava a imagem do jogo depois da reexecução,
ou seja, o intermediário.

### Por que foi removido
Testando na rota, no mato, na cidade e na captura (gravando o quadro intermediário e os vizinhos, quadro a quadro) os
erros vinham um atrás do outro: objetos iguais (canteiros, Pidgeys) pareados com o vizinho errado viravam fantasmas;
menus da captura em branco; modelos com esqueleto esticados; um clarão sobre a Poké Ball. Cada um tinha correção
(pareamento um a um por conteúdo, só desenhos 3D, sem paletas de ossos, limiar relativo), mas sempre aparecia outro
caso - sem as marcações por objeto que o RT64 recebe dos patches do jogo, o casamento entre quadros é um palpite.

## Investigação de 16/10/2026: identificar cada objeto pelo próprio jogo

O que faltava ao protótipo era saber **qual objeto é qual** entre dois quadros. Achado com `LARECOMP_DRAW_CHAINS` e
`LARECOMP_HOOK_DUMP` (+ `_FRAMES`), em Viridian, parado e andando (quadros 1450-1460):

- Por quadro o jogo grava 117 listas de comandos, faz ~407 `nvnCommandBufferCallCommands` (listas prontas) e só ~37
  draws diretos: quase todo objeto é desenhado chamando uma lista gravada antes.
- 356 dessas chamadas vêm de **`f_70006237b0`**, chamada por item pelo laço de `f_70006453a0`
  (`... f_7000644b50 → f_70006453a0 → f_70006237b0 → f_70005e3000 → CallCommands`). Ela é chamada 22 vezes por
  quadro, uma por objeto, com o **objeto em x0**.
- O ponteiro em x0 é **estável**: os mesmos 22 objetos em 10 quadros seguidos andando, nenhum repetido no quadro.
- A **ordem muda** a cada quadro (só de 2 a 14 dos 22 ficam na mesma posição): é por isso que parear pela ordem
  gerava fantasmas.
- Um segundo caminho, `f_7000641190` (via `f_7000642990`), desenha 7 objetos, cada um em dois passes por quadro
  (sombra/profundidade e cor): também estáveis; chave = objeto + passe.

Conclusão: com ganchos nessas funções, cada chamada de lista pode levar a identidade do objeto que o jogo está
desenhando, como os `gEXMatrixGroup` do RT64. O protótipo de interpolação (commit `9f2f5e5`, removido em `d72ab33`)
pode voltar pareando por objeto em vez de pela ordem; draws sem objeto (interface, efeitos de tela) ficam com os
valores do quadro atual.

## Fontes
- Ship of Harkinian, interpolação: [frame_interpolation.cpp](https://github.com/HarbourMasters/Shipwright/blob/develop/soh/soh/frame_interpolation.cpp)
- RT64: [repositório](https://github.com/rt64/rt64) e [comandos estendidos (rt64_extended_gbi.h)](https://github.com/rt64/rt64/blob/main/include/rt64_extended_gbi.h)
- Zelda64Recomp e alta taxa de quadros: [DSOGaming](https://www.dsogaming.com/news/the-legend-of-zelda-majoras-mask-just-got-an-unofficial-pc-port-with-support-for-high-framerates-widescreen-ultrawide-gyro-aim-autosaves-and-low-input-lag/), [Digital Foundry via Nintendo Life](https://www.nintendolife.com/news/2024/05/video-digital-foundry-takes-a-look-at-zelda-64-recompiled-majoras-mask)
- Sonic Unleashed Recompiled: [README](https://github.com/hedge-dev/UnleashedRecomp/blob/main/README.md) (seção "High Frame Rate Glitches")
- Mods de 60 fps no Switch (método do intervalo de present): [tutorial no GBAtemp](https://gbatemp.net/threads/how-to-make-60fps-ips-patch-for-nintendo-switch-game-ghidra-tutorial.625675/)
- Let's Go e 60 fps (velocidade dobrada): [GBAtemp](https://gbatemp.net/threads/request-lets-go-eevee-and-swsh-1-3-1-60-fps-mod.581515/)
- sm64ex: [Game Tech Wiki](https://emulation.gametechwiki.com/index.php/Super_Mario_64_Port), [VGC](https://videogameschronicle.com/news/mario-64s-pc-port-has-been-modded-to-run-at-60fps)

## O mod comunitário de 60 fps (Fl4sh9174), decodificado — 16/10/2026

Fonte: [Switch-Emulator-Ultrawide-FPS-Mods](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods) (commit `91db608`),
pacotes "Pokemon Let's Go Eevee" e "Pokémon Let's Go, Pikachu!" (clonado em `reference/fl4sh-mods`, fora do git).

| Pacote | Versão | Build ID do patch | Escritas (`@flag offset_shift 0x100`: endereço = offset no `main`) |
|---|---|---|---|
| Eevee, 60 FPS | **1.0.2** | `5831EC64D6B696FD69E69505821B97B1` | `005e78b8 0A000014` (= `b #+0x28`); `00f7eec0 2b50fe00` (u32 = 16.666.667) |
| Eevee, 120 FPS | 1.0.2 | idem | mesma escrita de código; `00f7eec0 e655a300` (u32 = 10.704.358, ≈93 Hz, **não** 8,33 ms) |
| Pikachu, 60 FPS | 1.0.2 | `C208DB6A4EF4361F28B8402BE0F7CD9A` | **mesmos** dois endereços e valores do Eevee |

Nosso dump é **v1.0.0** (Build ID `0a3f0d21e67dad80bb39beeaedbb0bab`): os Build IDs não batem, então nada vale por suposição.

- **Escrita de dados (`main+0xf7eec0`): confirmada no 1.0.0.** É `G[1]` da tabela de durações de quadro (`G[0]`=16.666.667,
  `G[1]`=33.333.334, `G[2]`=50.000.000 ... em `main+0xf7eeb8`). O mod troca 33,33 ms por 16,67 ms. É o mesmo que o
  `accessor_hook` de `runtime/hle/fps60.c` faz, só que lá a troca vale apenas quando o mundo lê `G[1]`; no mod vale para
  todo leitor, e é por isso que menu, batalha e captura aceleram (têm contadores de quadros inteiros, ver abaixo).
- **Escrita de código (`main+0x5e78b8`, `b +0x28`): NÃO mapeada.** No 1.0.0 esse endereço é um `blr x8` no meio de
  `f_70005e7890` (chamada de ponteiro NVN, slot `0x1603b88`); trocá-lo por `b +0x28` pularia o carregamento de `x21` e
  gravaria em `[x21+0x208]` com `x21` do chamador, o que não é o patch pretendido. Ou seja, o código do 1.0.2 está
  deslocado e a instrução original só se identifica com o `main` do 1.0.2, que não temos. A vizinhança é o encapsulamento
  de `nvnWindowGetPresentInterval/SetPresentInterval` (`f_70005e79c0`, slots `0xc38`/`0xc40`), então a hipótese é que o
  patch mexe no intervalo de present (de 2 para 1), mas **isso é hipótese, não medido**.
- O 120 FPS usa `G[1]`=10,7 ms, que não é 1/120: o efeito não é "taxa exata", e a relação não está explicada.
- Eevee e Pikachu 1.0.2 usam endereços iguais nos dois patches, mas isso foi só lido nos arquivos; não testamos o Pikachu.

## Mapa temporal (16/10/2026)

Corrige o "não há passo de tempo global" do cabeçalho e do PROGRESSO: há uma referência, mas só parte do jogo a usa.

**Quem lê a tabela `G`** (`main+0xf7eeb8`, via o ponteiro `0x70015fb438`; levantamento por `adrp`+`ldr` em todo o
`main`, 9 instruções):

| Função | O que faz com `G` | Roda no mundo? |
|---|---|---|
| `f_7000039600` (accessor) | `passo[i] = G[idx]` no objeto de temporizadores (chamada com `i = 1`, `idx = 1`, uma vez por atualização do mundo, de `0x70000393bc`) | sim, 1 por quadro |
| `f_7000663660`, `f_7000664cd0` | `passo / (G[idx] - 1)` = quadros a avançar (animações) | sim |
| `f_700067be50` | `passo / (G[0] - 1)` × uma velocidade por quadro | sim, 2 por quadro |
| `f_7000029d30` (fim do quadro) | `passo0 / (G[0] - 1)`, limitado a 1..5: só estatística (o valor vai para `f_70000358b0`, que não o usa) | sim |
| `f_7000038ae0` | copia `G[1]` para campos de um objeto na criação | não medido |
| `f_70000393e0` | `passo / (G[4] - 1)` | não chamado no mundo |
| `f_7000039520` | compara um tempo com `G[3]`, `G[2]`, `G[1]` (escolhe uma faixa) | não chamado no mundo |
| `f_700016d840` | `quantidade × G[1]` (quadros em nanossegundos) | não chamado no mundo |

**O objeto de temporizadores** (na pilha da thread principal, `0x723ffff450` nas execuções medidas): entradas de 16
bytes; o getter `f_70000395f0(obj, i)` devolve `obj + 8 + 16·i` e o passo fica em `+8` disso. Medido
(`LARECOMP_ELAPSED_LOG`, `LARECOMP_HOOK_DUMP` no accessor e no setter `f_7000039620`):
- passo 1: 33.333.334 ns a 30 fps; com `LARECOMP_FPS60=1` no mundo, 16.666.667 ns (o hook do accessor). Reescrito a
  cada atualização do mundo;
- passo 0: 33.333.334 ns sempre (inclusive no modo 60), gravado fora do accessor.
- O "tempo decorrido" das animações **não é medido**: é esse passo. Por isso o jogo não acelera nem desacelera com a
  carga, e trocar `G[1]` só no accessor muda exatamente quem usa o passo 1.

**Quem lê cada passo** (`LARECOMP_TIMER_USERS`, chamadas ao getter por endereço de retorno, 30 quadros no mundo e 90
na captura): os mesmos leitores nos dois casos.
- Usam o **valor** do passo 1: `0x70005ed1e8`/`0x70005ed908` (16 por quadro), `0x7000664d18`, `0x70006636a8`,
  `0x700067bf20`, `0x70006875e4`, `0x7000655634`.
- Usam o passo só como "atualiza ou não" (`> 0`): `0x700002333c` (~2.300 por quadro, quase tudo no passo 0),
  `0x700061640c` e `0x7000616494` (os dois passos, conforme o objeto).
- Passo 0 com valor: só `0x7000029e70` (a estatística do fim do quadro).

**Consequências**
- Mundo a 60 (`accessor` com `G[1]` = 16,7 ms): quem converte tempo em quadros pelo passo 1 anda meio quadro por
  atualização, na velocidade certa. Confirmado em jogo.
- O que avança **por atualização**, sem olhar o passo, dobra de velocidade a 60. Na captura e na batalha isso inclui as
  linhas do tempo da cena (os contadores achados com o rastreio de gravações: `0x7000b88340`, `0x7000b6acc8`,
  `0x70002d2a44`), o arco e a bola. Dividir esses três contadores (modo 2, `rt_scene_tick`) deixou o Pokémon na
  velocidade certa, segundo o teste em jogo, mas o arco e o arremesso continuaram rápidos: **há mais lógica por
  quadro que esses contadores não cobrem**. Por isso a opção sustentável para essas cenas é manter a lógica a 30 e
  apresentar a 60 (interpolação só de estados gráficos seguros).
- Hipóteses ainda não verificadas: o que grava o passo 0; se `f_70000393e0`/`f_7000039520`/`f_700016d840` rodam em
  batalha ou em outras cenas; se algum leitor de valor do passo 1 depende de ser exatamente 33,3 ms.

**Ferramentas** (runtime/hle/fps60.c, docs/DEPURACAO.md): `LARECOMP_ELAPSED_LOG="<função>:<passo>,..."` (+
`LARECOMP_TIME_PROBE_FRAMES`), `LARECOMP_TIMER_USERS=<de>-<até>`, `LARECOMP_TIME_PROBE`, `tools/teste60.sh`.
Cuidado: o `fnhook` aceita **um gancho por endereço** (o último registrado vale); duas sondas no mesmo endereço se
anulam (isso invalidou um teste do modo 2 com `LARECOMP_SHOT_REL`, já corrigido).

## Arquitetura adotada e validação da interpolação nas cenas (16/10/2026)

- **Mundo e menus**: 60 Hz nativos (passo 1 de 16,7 ms pelo accessor, "Mapa temporal").
- **Batalha e captura**: lógica a 30 Hz, como o jogo; apresentação a 60 com o quadro intermediário (`LARECOMP_FPS60=1`).
  A interpolação só mistura desenhos 3D com objeto identificado (`f_70006237b0`/`f_7000641190`), em alvo com
  profundidade e com teste de profundidade ligado; interface, transições e efeitos sem objeto ficam com o quadro atual
  (andam a 30). A mistura é por vec4, tudo ou nada.
- O modo 2 (contadores divididos) fica só como pesquisa: a batalha continua ~2× rápida (`tools/teste60.sh batalha 2`).

Validação (`tools/teste60.sh <cena> 1 LARECOMP_WINDOW_SHOTS_REL=1 LARECOMP_WINDOW_SHOTS=...` e
`tools/interp_metrics.py`): a métrica principal é a fração de pixels do intermediário que não existe em nenhum dos dois
quadros vizinhos num raio de 4 px (o que a interpolação inventou). Batalha: 42 de 47 quadros abaixo de 0,5%; os outros
são um corte de transição e giros rápidos de câmera, coerentes na imagem. Captura: nenhum quadro acima de 0,5%.
Pendente: o jogador confirmar em ritmo real; a interface e os efeitos sem objeto seguem a 30.

## Decisão (17/10/2026): sem interpolação

Testada em jogo depois das correções acima, a interpolação nas batalhas e capturas continuou com defeitos visíveis
para o jogador. Ela foi tirada do modo 60: `LARECOMP_FPS60=1` deixa mundo, menus e Pokédex a 60 nativos (passo 1 do
accessor) e batalhas e capturas a 30, como o jogo original, sem quadro intermediário. O código da interpolação fica
só para pesquisa (`LARECOMP_INTERP=1`); a escolha de 60 fps no launcher não a liga mais.
