# Ferramentas de depuração

Tudo o que foi usado para diagnosticar o port. As variáveis de ambiente valem para `build/lasrecomp.exe` (no Git Bash:
`LARECOMP_X=1 ./build/lasrecomp.exe extracted --play`).

## Execução e entrada
| Variável | Efeito |
|---|---|
| `LARECOMP_INPUT="frame:BOTAO:frames,..."` | entrada roteirizada (A, B, X, Y, L, R, PLUS, MINUS, direções, STICK_*). Desliga o controle real durante o teste |
| `LARECOMP_NPAD=handheld\|fullkey\|dual\|left\|right` | como o controle do PC aparece para o jogo (padrão: `handheld`) |
| `LARECOMP_FPS=<n>` | força n fps (padrão: o intervalo do jogo, 30 fps; `0` = sem limite) |
| `LARECOMP_NO_AUDIO=1` | sem áudio (testes automáticos) |
| `LARECOMP_FONT=<arquivo>` | fonte do sistema entregue ao jogo no lugar da lista padrão (`runtime/hle/pl.c`; a Yu Gothic Regular de alguns Win10 derruba o motor de fontes, issue #1) |
| `LARECOMP_LANG=en\|pt\|ru` | idioma do menu sobreposto (fechar o jogo, teclado do nome); sem ela, o idioma do sistema |
| `LARECOMP_SWKBD_UTF8=1` | teclado devolve UTF-8 em vez de UTF-16 |
| `LARECOMP_GYRO_TEST=x\|y\|z` | giroscópio virtual oscilando ±15° no eixo dado (`z+` / `x-`...: giro de 10° em rampa 1 s depois do início da mira); loga quando o jogo lê o sensor. Para chegar a uma captura: andar pela grama (`STICK_*`) e apertar A no "Get ready"; o encontro é sorteado, repita até o log ter `sensor started` |
| `mod` / `game_lang` (larecomp.ini) | mod ativo (`mods/<nome>/`, o log mostra `[mods]` e quantos arquivos do romfs foram trocados) / idioma pedido ao sistema (`en-US`, `es`...; um save existente mantém o seu) |
| `LARECOMP_LOG_FILES=1` | cada arquivo do romfs lido pelo jogo (descobrir onde está um texto ou imagem para um mod) |
| `LARECOMP_TOUCH=<de>-<até>[@x,y[,largura]]` | um dedo indo e voltando em volta de x,y (pixels da tela 1920x1080; largura 0 = parado) entre esses frames; `@x,y>x2,y2` arrasta; vários trechos com `;`. Loga `[touch]`: começo e fim do toque, gestos e quem lê. Brincar com Eevee: `1440:X:6,1490:UP:6,1530:A:6,1640:A:6,1700:A:6`; carinho na testa `1760-1960@960,375`; botão de frutas `@1807,952,0`. O dedo do controle no script: `RSTICK_*` e `ZR` |
| `SAVE=<savedata.bin ou pasta> tools/repro.sh ...` | roda o teste com outro save (copiado para `build/test-save`; o `save/` do jogador não é tocado). Os tempos do `LARECOMP_INPUT` mudam de um save para outro (tutoriais vistos ou não) |
| `TURBO=0 tools/repro.sh ...` | o `repro.sh` roda em turbo por padrão (`LARECOMP_FPS=0`, ~4x mais rápido: os quadros são os mesmos, só animações de espera ligadas ao tempo real mudam de fase); `TURBO=0` volta ao ritmo real, para medir fps ou áudio |
| `LARECOMP_DRAW_CHAINS=<quadro>[-<último>]` | registra cada draw, gravação de lista (`begin`) e chamada de lista (`call`) nesses quadros, com a cadeia de funções do jogo (`[chain]`) |
| `LARECOMP_HOOK_DUMP_FRAMES=<primeiro>-<último>` | limita o `LARECOMP_HOOK_DUMP` a esses quadros |
| `LARECOMP_ELAPSED_LOG="<função>:<passo>,..."` (+ `LARECOMP_TIME_PROBE_FRAMES=<de>-<até>`) | o passo de tempo (ns) que cada função recebe do objeto de temporizadores, com o objeto e o getter (docs/PESQUISA-60FPS.md, "Mapa temporal") |
| `LARECOMP_TIMER_USERS=<de>-<até>` | quem lê cada passo de tempo (chamadas ao getter `f_70000395f0` por endereço de retorno), listado no último quadro |
| `tools/teste60.sh <captura\|batalha> <0\|1\|2>` | a mesma cena a 30 fps e num modo 60, com imagens no mesmo tempo de jogo (`build/teste60/<cena>-<modo>/sheet.png`) |
| Atenção | o `fnhook` guarda **um gancho por endereço**: o último registrado vale; duas sondas na mesma função se anulam |
| `LARECOMP_DAYS_AHEAD=<n>` | relógio n dias à frente (coisas diárias: a fome do parceiro, que precisa ser maior que zero para ele comer) |
| `LARECOMP_WILDBATTLE=1` (+ `LARECOMP_WILDBATTLE_*`) | batalhas selvagens (docs/MODO-BATALHA.md; o launcher liga igual). `SPECIES=<n>`/`LEVEL=<n>`: sempre esse Pokémon (padrão: o tocado); `MOVES=a,b,c,d`: esses golpes (padrão: por nível); `MAX=<n>`: batalhas por execução (padrão: todas); `FADEIN=0` desliga o fade in manual, `FADE_TYPE`/`FADE_DELAY`/`FADE_FRAMES` o ajustam; `LOGPOKE=1` loga os campos do objeto do Pokémon (`=2` grava `build/poke_<quadro>.bin`); `STATE_LOG=1` os estados do processo; `DEBUG=1` grava os passes de um quadro 240 quadros depois da batalha; `USECFG=1` (experimental, derruba o jogo) lança a configuração do construtor |
| `LARECOMP_WATCH=<addr>[,<addr>...]` | vigia de escrita (Windows, `runtime/hle/memwatch.c`): protege a página desses endereços (palavras de 32 bits) e, a cada escrita neles, loga o backtrace de quem escreveu (funções do jogo como `f_<endereço>`). Os endereços do heap **não se repetem entre execuções** |
| `LARECOMP_WATCH_PARTY=<quadro>` + `LARECOMP_WATCH_SPECIES=133:10,16:9,...` | procura, no heap, registros de 0x104 bytes no formato PB7 (espécie u16 em `+8`, XP u32 em `+0x10`, nível em `+0xEC`) e vigia o XP de cada um. A heurística acha muitos falsos positivos (a estrutura em memória pode não ser PB7 cru) |
| `LARECOMP_SNAP=<quadro A>,<quadro B>` | copia as páginas não nulas do heap em A e, a cada 250 quadros a partir de B (30 vezes), lista os deltas pequenos mais comuns das palavras que cresceram (3 a 20 acertos) com endereços de exemplo; o ganho de XP de uma equipe é um delta compartilhado. O heap tem muito contador mudando, então o ruído é grande |
| `LARECOMP_GYRO_AXES=<yaw><pitch>` | eixos do giroscópio virtual movidos pelo mouse (padrão `zx`) |
| `mouse_sens` (larecomp.ini) | graus de giro por pixel do mouse (padrão 0,1) |
| `LARECOMP_LAUNCHER_KEYS=1` / `<slot>` / `<slot>:<tecla>` | launcher com a seção "Teclado e mouse" aberta / esperando a tecla da ação `slot` (`BIND_*` em runtime/input_bind.h) / apertando essa tecla (com `LARECOMP_LAUNCHER_SHOT`) |

## Travamentos e crashes
| Ferramenta | Uso |
|---|---|
| Backtrace do crash | funções do jogo aparecem como `f_<endereço>`; `svcBreak` mostra o `nn::Result` |
| `LARECOMP_THREAD_DUMP=<s>[,<repete s>]` | lista cada thread: svc em que está bloqueada, argumentos e pilha do guest |
| `LARECOMP_TRACE_SVC=1` | loga cada chamada de sistema (muito volume) |
| `LARECOMP_PROF_SVC=1` | a cada 2 s, por thread do guest: chamadas de sistema (`svc`), quantas e quanto tempo cada uma bloqueou (`[svcprof]`) |
| `LARECOMP_PROF_LOCKS=1` | a cada 2 s, os mutexes do jogo mais disputados (`ArbitrateLock` que teve de esperar): endereço, esperas, tempo e a pilha do guest da primeira (`[locks]`; resolver com `tools/addr2sym.py`) |
| `LARECOMP_GPUPROF_AT=<quadro>[,<quadro>...]` | tempo de GPU por passe, cópia e dispatch desses quadros (`[gpuprof]`; o cabeçalho traz o número do quadro). Com `LARECOMP_GPUPROF_DRAWS=1`, um carimbo antes de cada draw (uma linha por draw: vertex program, hash e endereço do fragment shader, índices, instâncias); com `LARECOMP_GPUPROF_STATS=1` também as estatísticas de pipeline de cada draw (`ia`, `vsinv`, `clipprim`, `fsinv`). Os carimbos serializam a GPU: servem para comparar draws entre si, e o total passa um pouco do real |
| `LARECOMP_EXP_VS=<endereço do vertex program, hex>` + `LARECOMP_EXP_MODE=skip\|scissor\|half` | pesquisa: nos draws desse programa, não desenha, desenha com scissor de 1x1 (sem pixels, só vértices) ou com metade das instâncias. `LARECOMP_EXP_ALT=1` só nos quadros ímpares, para ter as duas versões da mesma cena na mesma execução (ver [PESQUISA-GPU.md](PESQUISA-GPU.md): comparar execuções separadas engana) |
| `LARECOMP_EXP_VS_EARLY=<endereço>[:<pc, hex>]` | pesquisa: o vertex shader desse programa retorna no começo de `main`, ou antes da instrução no `pc` (o `pc` só existe no GLSL com `LARECOMP_SAVE_MSL=1`) |
| `LARECOMP_GLSL_OVERRIDE_DIR=<pasta>` | pesquisa: `<pasta>/<endereço do vertex program, hex>_vs.glsl` substitui o GLSL gerado (parta de `build/glsl/`, gravado por `LARECOMP_SAVE_MSL=1`) |
| `LARECOMP_NO_ROBUST=1` | pesquisa: desliga o `robustBufferAccess` do Vulkan |
| `LARECOMP_VERTEX_READ=direct\|copy\|auto` | de onde a GPU lê vértices e índices dos draws: `direct` = no lugar, da memória do jogo importada (pelo PCIe numa placa discreta); `copy` = cópia em VRAM; `auto` (padrão) = cópia em placa discreta, no lugar em integrada. Faixas que um compute escreve ficam sempre no lugar ([PESQUISA-GPU.md](PESQUISA-GPU.md)). A linha de estatísticas traz `vertex ranges N copied, M in place` |
| `LARECOMP_RENDER_THREAD=0\|1` | a thread de renderização do backend Vulkan (padrão ligada; `render_thread = 0` no `larecomp.ini` também desliga). `LARECOMP_INTERP` a desliga. `vertex_read = auto\|copy\|direct` no ini equivale a `LARECOMP_VERTEX_READ`. `LARECOMP_HEAL_KEYS=1`: F5 faz toda textura reconferir a memória, F6 lê todos os vértices no lugar (pesquisa, HISTORICO 60) |
| `LARECOMP_POOL_FIX=0` | volta ao comportamento antigo do pool de texturas (slot no objeto host liberado), só contando no log `[nvn] texture pool: ...` (HISTORICO 60) |
| `LARECOMP_INPUT_LOOP=<início>:<período>` | com `LARECOMP_INPUT`, repete as entradas de `[início, início + período)` para sempre; depois de cada batalha / captura fica parado 300 quadros, e dentro do encontro foge (B, três vezes para baixo, A). Usado por `tools/capture_loop.py run <nome>` / `scan <nome>` (pares antes/depois de cada encontro em `build/capture_loop/<nome>/`) |
| `LARECOMP_SHOT_RAW=<arquivo>` + `LARECOMP_SHOT_RAW_LAST=<quadro>` | a série `LARECOMP_SCREENSHOT_EVERY/FROM` vai para um arquivo cru (480x270 RGB por quadro, sem PNG) e o jogo sai no último quadro; lido por `tools/bateria_scan.py` |
| `LARECOMP_EXIT_AT_PRESENT=<n>` | com `LARECOMP_FRAMETIME_CSV`, sai no presente n; usado por `tools/perf_ab.py` |
| `EXE=<exe> tools/bateria.sh ...` / variante `B=EXE=<exe>,LARECOMP_GAME_LIB=<dll>` no `tools/perf_ab.py` | roda outra build do runtime (uma de referência compilada à parte, ex. `git worktree add ../lg-base HEAD` + `make build/lasrecomp.exe` lá, copiada para `build/base/`); na bateria, o Eevee usa então `build/lasgame.dll` pelo caminho. Uma DLL só carrega com o exe do mesmo `GAMELIB_ID` |
| `LARECOMP_PIPE_STATS=<vertex program, hex>\|all` | o que o driver informa de cada shader compilado (registradores, spills, tamanho do código: `[pipestats]`) e o desenho dos atributos de vértice do programa (`VK_KHR_pipeline_executable_properties`) |
| `LARECOMP_LOG_VCOPY=1` (com `LARECOMP_GPUPROF_AT`) | uma linha por cópia de stream de vértice nos quadros perfilados: bytes pedidos, tamanho do buffer ligado, stride, índice máximo |
| `LARECOMP_LOG_GPUWRITES=1` | loga cada faixa de memória que um compute pode escrever (storage buffers ligados) ao entrar no conjunto |
| `LARECOMP_HLE_WHO=<endereço falso, hex>[,...]` | no boot, imprime o nome da função do host por trás de um endereço `0x70800000xx` visto num `LARECOMP_THREAD_DUMP` |
| `LARECOMP_LOCK_SPIN_US=<n>` | quanto uma thread espera girando por um mutex contendido antes de dormir (padrão 40; 0 = sempre dorme, como antes; desligado com menos de 4 processadores lógicos). Para comparar antes/depois no mesmo binário |
| `tools/addr2sym.py <endereço> [-d <bytes>]` | módulo, símbolo mais próximo e disassembly de um endereço do guest |
| `tools/addrmap.py <main de> <main para> <endereço>... [d:<dado>]` | o mesmo código (ou dado) em outro build do `main` (Eevee <-> Pikachu, atualização): compara as instruções com os campos relativos mascarados até achar uma só. Os endereços do runtime vão em `runtime/fnhook.c` e `tools/patches.py` (regra de paridade, CLAUDE.md) |
| `nm build/lasrecomp.exe` | símbolos do runtime para endereços `lasrecomp+0x...` |

## Gráficos
| Variável | Efeito |
|---|---|
| `LARECOMP_SHOT_AT=<frame>,...` | salva o quadro do jogo em `build/frames/frame_N.png` |
| Estatística a cada 2 s (`[vk] t=...`) | draws, passes, dispatches, uploads, `compile N (translate, spirv, pipelines, draws waited)` (programas e pipelines prontos, o tempo de CPU de cada etapa e quanto os draws esperaram por eles). Para medir uma primeira visita, renomeie `cache/` antes do teste e devolva depois e `[skip: ...]` com os draws pulados por motivo |
| `tools/perf_summary.py [log]` | resumo de uma sessão: faixa de fps, quadros acima de 50 ms, draws pulados, compilações, underruns de áudio (padrão: `logs/jogo.log`) |
| `LARECOMP_FRAMETIME_CSV=<arquivo>` | grava `present,dt_ms` para cada present do jogo (exceto o primeiro), com tempo monotônico; quadro intermediário extra do menu a 60 não conta. Usar um caminho em `build/` |
| `tools/frametime_summary.py <csv> [--from-present N]` | média, FPS, P95/P99 (nearest rank), máximo e número de presents acima de 50 ms; filtrar o início para separar boot/loading do mundo |
| `LARECOMP_VK_VALIDATION=1` + `VK_ADD_LAYER_PATH=C:\msys64\ucrt64\bin` | camada de validação do Vulkan (achou o overflow das texturas do vídeo) |
| `LARECOMP_VK_DEVLOST_TRACE=1` (+ `LARECOMP_SYNC_SUBMIT=1`) | últimas operações da GPU antes de um device lost |
| `LARECOMP_SAVE_MSL=1` | salva cada shader traduzido em `build/glsl/<addr>_{vs,fs}.glsl` |
| `LARECOMP_DEBUG_FRAME=<frame>` | lista todos os draws daquele quadro (programa, formatos de atributo, render targets e, por textura: tamanho, formato, flags, altura de bloco, endereço) |
| `LARECOMP_LOG_SKIN=1` (+ `LARECOMP_DEBUG_VSCB=1`) | detalha draws com esqueleto: vértices, índices/pesos dos ossos, estatística da paleta |
| `LARECOMP_DUMP_SKIN=<n>` | grava as entradas do n-ésimo draw com esqueleto em `build/skin/` |
| `LARECOMP_VK_UBO_DIRECT=1` | uniforms lidos direto da memória do jogo na GPU (experimento) |
| `LARECOMP_VK_EXACT_H2F=1` | meia precisão (half) convertida bit a bit em todos os estágios, como antes; sem ela só os vertex shaders fazem isso e fragment/compute usam `unpackHalf2x16` (HISTORICO 63) |
| `LARECOMP_VK_COPY_VERTICES=1` | força a cópia de **todos** os vértices e índices, até os que um compute escreve (podem ficar velhos); para o uso normal, `LARECOMP_VERTEX_READ` |
| `LARECOMP_NO_SCALE=1` | desliga a escala de resolução interna |
| `LARECOMP_SCALE=<f>` | altura de render = 1080 x f (ignora o launcher). Com `LARECOMP_FPS=0` mede a folga da GPU |
| `LARECOMP_SETTINGS=<arquivo>` | outro `larecomp.ini` (ex.: `smaa = 0` para comparar sem SMAA) |
| `LARECOMP_ANISO=<n>` | filtragem anisotrópica forçada (padrão 16; 0 ou 1 = a do jogo) |
| `LARECOMP_SHADER_WAIT_MS=<ms>` | quanto um quadro pode esperar por shaders / pipelines em preparo antes de pular draws (padrão 20; 0 = nunca) |
| `LARECOMP_HIDE_FIRST_FRAMES=<n>` | janela preta nos n primeiros presents (padrão 2: o flash azul da abertura; 0 mostra tudo) |
| `LARECOMP_WINDOW_SHOT=<frame>` | grava a imagem **da janela** em `build/window.png` (o `LARECOMP_SHOT_AT` grava a imagem do jogo, antes da janela) |
| `LARECOMP_LOG_CLEARS=1` | cada limpeza de cor: frame, alvo, formato, máscara e a cor |
| `LARECOMP_DUMP_TEX_FMT=<formato NVN>` | grava o nível 0 das texturas daquele formato, já sem tiling, em `build/tex/<addr>_<w>x<h>_f<fmt>_row<bytes>.bin` |
| `LARECOMP_VK_SKIN_ONEBONE=1` | shaders com esqueleto usam só o 1º osso (experimento X4; foi o paliativo antes da correção) |
| `LARECOMP_VK_SKIN_IDENTITY=1` / `LARECOMP_VK_SKIN_IDX_X=1` / `LARECOMP_VK_SKIN_W=<expr>` | outros experimentos do [PLANO-3D.md](PLANO-3D.md) |
| `LARECOMP_DUMP_SKIN=1` + `LARECOMP_DUMP_SKIN_FRAME=<frame>` | grava o 1º draw com esqueleto a partir daquele frame em `build/skin/` |

### Harness de shader na CPU (`tools/skintest`)
Roda um vertex shader traduzido na CPU com os dados reais de um draw:
```
LARECOMP_SAVE_MSL=1 LARECOMP_DUMP_SKIN=400 ./build/lasrecomp.exe extracted --play
./.venv/Scripts/python tools/skintest/make_harness.py build/glsl/<vs>_vs.glsl build/skintest.cpp
clang++ -O1 -std=c++17 -w -o build/skintest.exe build/skintest.cpp && ./build/skintest.exe build/skin
```
O `.cpp` gerado contém o shader do jogo: fica em `build/` e nunca vai para o git. Se a pasta tiver um `gpu_pos.bin` (exportado por
`tools/rdc_draw.py`), cada vértice também mostra a posição que a GPU calculou.

### RenderDoc (o que a GPU realmente recebeu)
RenderDoc 1.46 em `C:\Program Files\RenderDoc`. O runtime dispara a captura sozinho quando roda dentro dele:
```
RENDERDOC=1 tools/repro.sh rdc 1310 LARECOMP_RENDERDOC_AT=1290 [LARECOMP_RENDERDOC_FRAMES=2]
```
Isso grava `build/rdc/capture_frameN.rdc` (4 frames seguidos por padrão, porque o jogo não desenha a cena inteira
em todo present) e `build/rdc-shaders.txt`, com o hash de cada SPIR-V e o endereço do shader no jogo. Para analisar,
use o Python do `qrenderdoc`. Ele trava na inicialização se achar o depurador Python do VS Code no perfil do
usuário, então aponte `USERPROFILE`/`HOME` para uma pasta vazia **fora** de `build/rdc` (o `repro.sh` apaga essa pasta):
```
H="$(cygpath -w "$PWD/build/rdc-home")"
RDC_DIR=D:/projects/letsgo-recomp/build/rdc OUT=D:/projects/letsgo-recomp/build/rdc/skin.txt   USERPROFILE="$H" HOME="$H" "/c/Program Files/RenderDoc/qrenderdoc.exe" --python D:/projects/letsgo-recomp/tools/rdc_skin.py
RDC=<captura.rdc> EID=<evento> OUT=<arquivo.txt> USERPROFILE="$H" HOME="$H"   "/c/Program Files/RenderDoc/qrenderdoc.exe" --python D:/projects/letsgo-recomp/tools/rdc_draw.py
```
- `rdc_skin.py`: para cada draw com esqueleto, mostra o shader, a faixa de NDC e de `w` e quantos vértices saem da tela.
- `rdc_draw.py`: um draw vértice a vértice (`a11`, `a12`, `gl_Position`), separando os vértices com `w` fora da
  mediana. Exporta `build/rdc/draw_<eid>/` no formato do harness (`verts.bin`, `cbN.bin`, `gpu_pos.bin`).

Não ligue debug info nos shaders para capturar: com milhares de shaders, o replay leva mais de 20 minutos só para abrir.

### Funções do jogo: perfil, comparação e ganchos
- `LARECOMP_FNPROF=<de>:<até>` grava em `build/fnprof.txt` (ou `LARECOMP_FNPROF_OUT`) as funções do jogo que
  rodaram nesses frames: endereço, chamadas, ordem da primeira entrada e o `lr` dessa entrada (runtime/fnprof.c).
  `LARECOMP_FNPROF_BT=<addr>[,...]` (+ `LARECOMP_FNPROF_BT_MAX=<n>`) imprime a pilha (funções `f_<addr>`) nas
  primeiras entradas dessas funções.
- `tools/fncompare.py --base <perfis sem o evento> --event nome=<perfil>` lista o que só o evento executou; com dois
  eventos, o que é comum e o que é de cada um (exemplo: docs/MODO-BATALHA.md).
- **Ganchos** (runtime/fnhook.c): toda função gerada passa por `FN_ENTER`, que chama `rt_fnhook` enquanto houver
  gancho registrado (`rt_register_fnhook(addr, fn)`; `fn` devolve 1 para tratar a chamada no lugar do jogo;
  `rt_call_original` roda a função original de dentro do gancho). Mudar comportamento não exige gerar a DLL de novo.
- `LARECOMP_SCAN="<bytes hex>[@<frame>]"` (+ `LARECOMP_SCAN_BACK=<n>`): varre a memória mapeada do guest por esses bytes
  (ex.: o hash de um registro em little-endian) quando o jogo chega ao frame (padrão 1300) e loga cada acerto, os 0x40
  bytes seguintes, `n` registros de 0x40 antes e os ponteiros para a base (runtime/hle/memscan.c, só Windows).
- `LARECOMP_HOOK_DUMP="<addr>:<args>[;...]"` (endereço completo do guest, ex.: `700028ef70`; o offset do `main`
  sozinho não dispara) registra, nas primeiras `LARECOMP_HOOK_DUMP_MAX` (4) entradas, x0..x7 e a
  memória dos argumentos: `x2:30` = 0x30 bytes em x2; `*x0:400` = 0x400 bytes no ponteiro guardado em x0.

### Referências cruzadas no código do jogo (`tools/xrefs.py`)
`cd tools && ../.venv/Scripts/python xrefs.py calls <addr>` (quem chama a função que contém o endereço), `imm <valor>`
(instruções com esse imediato) ou `float <valor>` (onde a constante está e quem a carrega). A desmontagem do `main`
fica em cache em `build/xrefs-main.pkl`. Chamadas por vtable não aparecem: para essas, a cadeia de chamadores em tempo
de execução (frame pointers do guest, `x29`) é o caminho.

### Jogar enquanto se testa
`build/jogo/lasrecomp.exe` (dois cliques) é a cópia estável. `make jogo` (`tools/publicar_jogo.sh`, com o jogo
fechado) copia o build atual para lá depois de validado. Assim, recompilar e testar em `build/` não esbarra no executável travado pelo jogo aberto.

## Vídeo e áudio
| Variável | Efeito |
|---|---|
| `LARECOMP_AUDOUT_WAV=<prefixo>` | grava cada saída de áudio em `<prefixo>.<n>ch.raw` (s16, 48 kHz) |
| `LARECOMP_AUDOUT_MS=<ms>` | fila mínima da saída de áudio (padrão 60) |
| `LARECOMP_MOVIE_STATE=1` | estado das filas dos decoders a cada 5 s |
| `LARECOMP_LOG_MOVIE_CALLERS=1` | quem lê os quadros do vídeo (Buffer::Base, GetOutputBuffer) |
| `LARECOMP_MOVIE_MAX=<w>x<h>` | reduz vídeos maiores que isso |
| `LARECOMP_MOVIE_TEST_FILL=<byte>` | preenche os quadros do vídeo com um valor (teste) |
| `LARECOMP_LOG_BUF2TEX=1` | log das cópias buffer → textura dos planos de vídeo |

Para comparar áudio com o original:
`ffmpeg -i <video>.webm -vn -f s16le -ar 48000 -ac 2 ref.raw` e correlacionar com a gravação (numpy no `.venv`).
