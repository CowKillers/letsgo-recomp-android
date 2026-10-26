# Modo de jogo: batalhar com Pokémon selvagens

_Ideia (15/10/2026): ao encostar num Pokémon selvagem, em vez de ir direto para a captura (estilo Pokémon GO), entrar
numa batalha clássica - e capturar depois, como o jogo já faz com os lendários e o Snorlax. Até onde se sabe ninguém
fez isso em mod do Let's Go ([discussão de 2026](https://reddit.sentinel-team.org/posts/1rr2xif/snapshots/2026-03-13T05%3A19%3A13.96841Z))._

## Etapa 1: mapa dos dois caminhos ✅

Método: `LARECOMP_FNPROF` (runtime/fnprof.c: funções do jogo executadas numa janela de frames, agora com a ordem da
primeira entrada e o chamador) em rodadas sem encontro, com encontro selvagem e com batalha de treinador (save na
Rota 1, ao lado do Youngster Ronny); `tools/fncompare.py` separa o que é comum e o que é de cada evento, e
`LARECOMP_FNPROF_BT=<addr>` + `LARECOMP_FNPROF_BT_MAX=<n>` mostram a cadeia de chamadas.

Atualização do mapa (todo quadro): `main+13c8f0` → `13d2f0` → `315390` → `2f78d0` → **`2f8bb0`**, que chama:
- **`2fda00`** (Pokémon selvagens no mapa). No encostar: → **`305ad0`** → **`28ef70`**`(x0 = objeto do encontro,
  x1 = gerenciador global [*0x70015faab8]+0x68)`, que aloca a cena de captura (0x3B0 bytes) e a empurra:
  `31c030` → `139e80` (empilhar processo/cena).
- **`2facf0`** / `2fa860` (visão dos treinadores). Ao ver o jogador: `17a7f0` → `17aa70` → `17b380` → `139e80`
  (evento: "!" e diálogo).

Batalha de treinador, depois do diálogo: interpretador de script `17b6c0` → `19ca10` → `19b7f0` → comando
`674ed0` → `672484` → `66fc20` → `17e150` → **`28bb60`**`(x0 = &objeto, x1, x2 = &parâmetros)`, que copia uma
configuração de batalha de **0x30 bytes** para o objeto da batalha (+0x88) e troca de cena (`28bc30` → `28e480` →
`139e80`); dentro da batalha, `28be80` → `28ccb0` → `28b150` → `31a470`.

Conclusão: captura (`28ef70`) e batalha (`28bb60`/`28e480`) são métodos vizinhos do mesmo gerenciador de encontros
(`main+28xxxx`). O desvio seria em `305ad0`: montar uma configuração de batalha selvagem para o Pokémon tocado e
chamar o início de batalha no lugar da captura; depois da batalha, iniciar a captura (o fluxo dos lendários).

## Etapa 2: ganchos e primeiros dados ✅

- **Ganchos de função** (runtime/fnhook.c, `FN_ENTER` em runtime/cpu.h, ABI da DLL 3): o runtime pode tratar ou
  envolver qualquer função do jogo sem gerar a DLL de novo. `LARECOMP_HOOK_DUMP` registra argumentos e memória.
- **Batalha do Youngster Ronny** (`28bb60`, chamado só pelo comando de script `17e150`):
  - `x2` = parâmetros de 0x30 bytes: `{0, 0, 0x1C1, 0, 0x200, ...}`; o comando de script recebe os mesmos 0x1C1 e
    0x200 - **0x1C1 (449) deve ser o treinador**, 0x200 flags;
  - `*x0` = objeto grande com contagem de referências (vtable `0x7001537770`): o pedido de batalha do evento.
- **Outros caminhos** (referências diretas no `main`, `tools/xrefs.py`):
  - `28bc30` (inicia a batalha de fato) tem dois chamadores: `28bb60` (script) e **`28bda0`**, chamado por
    `289c4c` - um método virtual que monta os parâmetros com um **modo 0 ou 3** conforme a flag `[obj+0xA6]`;
  - a captura `28ef70` tem quatro chamadores: o encontro selvagem (`305ac4`), `28ea90`, `854a48` e **`31a0e0`**,
    chamado pelo método virtual `319e20`, ao lado da sequência de batalha (`31a470`) - candidato a "batalha e depois
    captura" dos lendários.

## Etapa 3 (parcial, 17/10/2026): o que a pesquisa estática e os dumps mostraram
**Chamadores conferidos** (busca de `bl` direto para o endereço exato; o `xrefs.py calls` erra o início da função
quando há padding antes dela):
- `28ef70` (captura): `28ead8`, `305b34` (encontro selvagem), `31a140` (fn `31a0d8`), `854afc`. Confere com a etapa 2.
- `28bc30` (iniciar batalha): só `28bb80` (script) e `28bdbc`; `28bb60` só `17e218`; `28bda0` só `289ce0`.

**`289c4c` e `319e20` não são métodos soltos**: são casos da jump table de máquinas de estado (estado em
`[obj+0x68]`) de duas classes com vtable própria. Classe A: vtable em `image+0x1546ea8` (slot da GOT `0x15ff050`),
construtores a partir de `289ac0`, fábrica `17f098` (aloca `0xA8` bytes). Classe B: vtable em `image+0x154cb70` (GOT
`0x15ff8c0`), construtor `318f50`, fábrica `291470` (aloca `0x100` bytes). As fábricas seguem o mesmo molde (alocar,
construir, empilhar com `13a750(gerenciador, &objeto)`), o que sugere uma família grande de "processos".
- Classe A, caso `289c4c`: monta `{u32 modo, ponteiro config}` com modo `3` se `[obj+0xA6] != 0`, senão `0`, config =
  `[obj+0x90]`, e chama `28bda0`, que inicia a batalha (`28bc30`) e grava modo em `battle+0xb8` e config em `+0xc0`.
  Depois chama `28dd20`/`28dd40` com as flags `[obj+0xa0..0xa2]`.
- Classe B, caso `319e20`: chama `31a0e0` (que chama a captura `28ef70`) e devolve estado 4.
- Ainda **não confirmado em execução** que A + B formem o fluxo "batalha e depois captura" dos lendários, e não sei
  quem instancia as fábricas (são métodos virtuais, sem `bl` direto).

**Dump de um encontro selvagem real** (`LARECOMP_HOOK_DUMP`, frame 1545 da Rota 1): `305ad0` recebe `x0` = objeto do
Pokémon (vtable `0x70159c848`, campos `+8 = 2`, `+0x28 = 4`), `x1` = gerenciador; `28ef70` recebe `x0` = objeto de
vtable `0x7001537770` (**a mesma classe do "pedido de batalha" do `28bb60`**) e `x1` = alocador do gerenciador global.
Quem identifica a espécie/nível do Pokémon tocado ainda não foi lido.

**Armadilhas dos testes** (para não repetir):
- `LARECOMP_HOOK_DUMP` exige o endereço **completo do guest** (`700028ef70`), não o offset do `main` (`28ef70` não
  dispara nada, sem erro).
- O `fps60.c` registra ganchos em `28ef70` e `28bb60` quando `LARECOMP_SHOT_REL` está ligado, e o `teste60.sh` sempre
  liga: o último gancho registrado vale, então o dump é anulado. Use `tools/repro.sh` direto com o roteiro.
- O sorteio do encontro muda o resultado do mesmo roteiro: o "captura" às vezes cai numa batalha de treinador e o
  "batalha" às vezes numa captura. Confira a imagem final ou o log antes de concluir.
- Worktree: `extracted/`, `generated/` e `.venv/` entram como junctions; copie `build/lasgame.dll` **e
  `build/lasgame.dll.id`** (sem o `.id` o runtime recusa a DLL: "built for another version") e compile só o runtime
  (`make -j20 build/lasrecomp.exe`).

## Etapa 3b (17/10/2026): o que a comunidade já tem, e a tabela das batalhas "estáticas"
**Comunidade** (pesquisa em 17/10/2026): o [pkNX](https://github.com/kwsch/pkNX) edita o Let's Go por **dados do
romfs** (caminhos em `pkNX.Game/File/GameFileMapping.cs`, `FilesGG`): encontros estáticos
(`bin/script_event_data/event_encount.bin`), presentes (`add_poke.bin`), trocas (`field_trade_data.bin`), tabelas
de selvagens (`bin/field/param/encount/encount_data_{p,e}.bin`, FlatBuffers `wild.fbs`), treinadores
(`bin/trainer/trainer_{data,poke,type}`). Taxa de shiny, TMs e tabela de tipos ficam no `main` e o pkNX os acha por
padrão de bytes e os altera (`EditorGG.cs`). O mod de dados [Peerless Pikachu & Exquisite Eevee PLUS](https://www.pokecommunity.com/threads/pok%C3%A9mon-lets-go-peerless-pikachu-exquisite-eevee-plus.541476/)
(feito com pkNX) mexe em selvagens, estáticos e treinadores, mas na [discussão do lançamento](https://reddit.sentinel-team.org/posts/1rr2xif/snapshots/2026-03-13T05%3A19%3A13.96841Z)
um comentário diz que ninguém achou como pôr batalhas selvagens comuns, e vários pedem isso. Ou seja: **editar a tabela
dos estáticos é conhecido; transformar um selvagem comum em batalha ninguém fez** (e nada nas fontes diz que seja
impossível).

**A tabela das batalhas estáticas, no nosso romfs** (`extracted/romfs/bin/script_event_data/event_encount.bin`, 10
registros de 0x40 bytes; layout `EncounterStatic7b` do pkNX: `+0` hash u64, `+8` espécie u16, `+0xA` forma, `+0xB`
nível, `+0xC` shiny, `+0xD` gênero, `+0xE` natureza (25 = aleatória), `+0xF` habilidade, `+0x20` 4 golpes u16, `+0x2C`
IVs, `+0x32` EVs, `+0x38` aumentos de status, `+0x10..0x1F` ponteiros de texto):
| # | Pokémon | Nível |
|---|---|---|
| 0 / 1 | Pikachu (forma 8) / Eevee (forma 1) iniciais | 5 |
| 2 / 3 / 4 | Articuno / Zapdos / Moltres | 50 (+1 em todos os status) |
| 5 | Mewtwo | 70 (+1 em todos) |
| 6 / 7 | Snorlax (dois, +1 Atk / +1 Def) | 34 |
| 8 / 9 | Voltorb / Electrode | 42 |
Cada batalha é achada por um **hash u64** que vem do script do evento. Isto é o "pedido de batalha selvagem" que
procurávamos: a espécie, o nível, o gênero, a natureza, os IVs e os golpes de um selvagem cabem num registro assim.

Pistas no `main` (tabela de strings perto de `image+0xf04100`): nomes de cenários `sinario_densetu_mae` e
`sinario_densetu_hokaku` (densetsu = lenda, hokaku = captura) ao lado dos três caminhos de arquivo acima. O código não
referencia os nomes por `adrp`+`add` nem por ponteiro direto (a busca não achou nada), então o carregador usa índices.

**Plano revisado do protótipo**: em vez de montar o pedido de batalha do zero, reaproveitar o comando de script das
batalhas estáticas com um **registro sintético** (copiar o do Snorlax e preencher espécie, forma, nível, gênero,
natureza, IVs e shiny do Pokémon tocado; os golpes vêm da tabela de aprendizado pelo nível). Falta achar: o carregador
da tabela (a tabela fica em memória depois de lida: dá para achar o endereço varrendo o heap pelo hash do Snorlax
`343e35139d576e27`) e o comando de script que recebe o hash. Outro caminho já provado pela comunidade: cheats e mods
de "teletransporte" e de "lendário aparecer" mexem em posição e em flags (`bin/flagwork`); como a memória do guest é
memória do host, o runtime pode fazer o mesmo para **chegar a uma batalha nativa de Snorlax/lendário** e observar o
fluxo de verdade, sem save adiantado.

## Etapa 3c (17/10/2026): quem lê a tabela por hash = a classe A, o processo das batalhas estáticas
(Correção: os "leitores" do `event_encount.bin` aparecem por `adrp`+`add` no início da **string** `+0x156` da tabela de
strings em `image+0xf04000`, e não no ponto onde o nome do arquivo começa dentro dela, que foi o meu erro na busca.)

A tabela **não está na memória durante o campo**: só `field_trade_data.bin` é lido no boot (`LARECOMP_LOG_FILES=1`);
`event_encount.bin` e `add_poke.bin` são lidos **sob demanda**, quando um evento os pede. Varredura de memória do guest
pelo hash do Snorlax no frame 1400 da Rota 1: 0 acertos (`LARECOMP_SCAN`, ver abaixo).

O código, no `main`:
- **Carregador da tabela** (classe com objeto de `0x140` bytes, ctor `2ba800`): `2ba920(this)` pede o arquivo
  `bin/script_event_data/event_encount.bin` (string `+0x156`, via `70001d5150` e `70000a63d0`); `2baae0` diz se já
  carregou (`1` quando `[this+0x130]` ou o arquivo pronto); `2bab70` percorre `tamanho >> 6` registros de 0x40 bytes,
  converte cada um numa estrutura de 0x59 bytes (hash, espécie, forma, nível, shiny, gênero, natureza, habilidade,
  golpes, IVs, EVs, aumentos) e insere num `unordered_map` com chave = hash (`2baea0`). **`2bad00(tabela, &chave)`** é a
  busca: devolve `nó + 0x18` (o ponteiro para a estrutura). Usado só pelas classes dos estados `289b40` (A), `28e99c`
  (`28ea90`, que chama a captura `28ef70`) e `28f454`.
- **Classe A** (vtable `image+0x1546ea8`, objeto de `0xA8` bytes, fábrica `17f098`) é o processo da batalha estática:
  `[+0x68]` estado; `[+0x88]` objeto da tabela; `[+0x90]` configuração de batalha (`0x2b8` bytes); **`[+0x98]` o hash
  (u64) da batalha**; `[+0xa0..0xa2]` e `[+0xa6]` flags. Estados: 0 (`289b7c`) cria o objeto da tabela e pede o arquivo;
  1 (`289e18`) espera (`2baae0`); 2 (`289e40`) aloca a configuração, **busca por `[+0x98]`** (`2bad00`) e preenche a
  configuração com `2f60c0(config, registro)` (ela grava `[config+0x298] = 1` e copia espécie, nível etc.); 3 (`289c4c`)
  inicia a batalha (`28bda0`, modo 3 se `[+0xa6] != 0`, senão 0) e depois `28dd20`/`28dd40`; 4 não lido.
- A fábrica `17f098` é chamada de um método virtual (o comando de script do evento), que recebe o gerenciador de
  processos e o alocador e depois preenche o hash e as flags no objeto devolvido (a confirmar).

**Consequência para o protótipo** (duas opções, a segunda é a mais limpa):
1. Criar a classe A pela fábrica `17f098` e gravar o hash do Snorlax em `[obj+0x98]` (batalha nativa de teste).
2. **Gancho em `2bad00`**: se a chave for um hash nosso, devolver um registro sintético (estrutura de 0x59 bytes,
   mesma disposição do parser) com a espécie, nível, gênero, natureza e IVs do Pokémon tocado; o resto do fluxo
   (configuração, batalha, captura depois) é o do jogo.

**Ferramenta nova**: `LARECOMP_SCAN="<bytes hex>[@<frame>]"` (+ `LARECOMP_SCAN_BACK=<n registros>`) varre a memória
mapeada do guest (Windows, `runtime/hle/memscan.c`) e loga cada acerto, os 0x40 bytes e quem aponta para a base.

> **Estado em 17/10/2026:** funciona (experimental). Resumo, sintomas e correções no [HISTORICO.md](HISTORICO.md), seção
> "Modo batalhas selvagens". As etapas abaixo são o diário da pesquisa.

## Etapa 4 (17/10/2026): o protótipo funciona; os nativos de script do jogo
**Protótipo** (`runtime/hle/wildbattle.c`, `LARECOMP_WILDBATTLE=1`): o gancho no `305ad0` (o gatilho do toque num
selvagem, que chama a captura e depois configura o handle devolvido, por isso é substituído inteiro) cria o processo da
classe A pela fábrica `17f0a0`, o gancho no construtor `289ac0` grava o hash nosso em `[+0x98]` e o gancho em `2bad00`
responde a esse hash com uma cópia do registro real do Snorlax (molde) com espécie, nível e golpes trocados. Jogado por
um humano na Rota 1: encostar num Pokémon abre a batalha (Rattata nível 3 de teste) e, ao fim, a captura da espécie da
batalha. Problema aberto: tela preta ao voltar ao mundo (o mundo é desenhado, os passes finais saem pretos: um fade).

**Os nomes dos nativos de script estão no `main`** (strings em `image+0xeff9xx..0xf01dxx`, ponteiro para a função no slot
seguinte da tabela de nativos): `CallTrainerBattleCore` `17e150`, **`CallWildBattle` `17e440`**, `GetTrainerBattleResult_`
`17e700`, `GetWildBattleResult_` `17e7c0`, `CallBattleLose_` `17e880`, `SaveEventEncountPokemonParam` `17ed00`,
`ClearEventEncountPokemonParam` `17ee70`, `DisappearEncountSymbols` `185cc0`, `EncountPokeSetVisible` `189fe0`,
`FadeIn_` `1a7680`, `FadeOut_` `1a7860`, `FadeWait_` `1a7aa0`, `FadeCheck_` `1a7ad0`. Os scripts são AMX (Pawn) em
`bin/archive/script_amx.gfpak` (GFLXPACK: 705 arquivos, LZ4; formato no `pkNX.Containers/Misc/GFPack.cs`) e **não guardam
nomes**, só hashes.
- **`CallWildBattle(amx, params)`**: params = `{[1] hash u64, [2] flags u32}`. Cria a classe A (fábrica `17f0a0`) e grava
  `[+0x98] = hash`, `[+0xa0] = [+0xa1] = 0`, `[+0xa2] = 1`, `[+0xa3] = !(flags>>3 & 1)`, `[+0xa4] = !(flags>>1 & 1)`,
  `[+0xa5] = !(flags>>2 & 1)`, `[+0xa6] = 1` só num caso especial (senão 0), e suspende o script (`672e50(amx, 0xc)`) até o
  processo acabar. Os padrões do **construtor** (`a0 = a1 = 1`, `a2 = 0`) são o oposto: o primeiro protótipo os deixava assim.
- `FadeIn_(amx, params)`: `{[1] quadros, [2] hash FNV-1a 64 do nome do tipo}` -> índice 1..11 -> função do motor
  `11ece0(gerenciador, 0, tipo, quadros)`; `FadeOut_` termina em `11ee50(gerenciador, 0, tipo, ...)`; gerenciador de fade
  = `[[*GOT 0x70015facf8]]`. Nomes dos tipos (strings em `image+0xefd685..0xefdb2e`): `fade_front_{fade,wipe,r,trainer,pika,
  bui,poke_get,legend,legend_get,loading}_{in,out,keep}`.
- O script faz FadeOut -> CallWildBattle -> (espera) -> GetWildBattleResult_ -> FadeIn..., o que o protótipo não faz.

## Etapa 5 (17/10/2026): XP dividido, o que se sabe e o que não deu
Objetivo: metade do XP ao "derrotar" (no fluxo dos lendários o Pokémon não desmaia: a batalha termina e começa a captura) e
a outra metade na captura. **Não implementado.** O que a pesquisa trouxe:
- A tela de recompensa da captura mostra o XP: **Metapod nível 5 = 20**, **Pidgey nível 4 = 11** ("Bonus Multiplier 1.0,
  Successful Catch", "Exp. Points"); depois vem a tela da equipe ("Oddish grew to Lv. 9!"). Uma batalha de treinador mostra
  "Your party Pokémon in waiting also got Exp. Points!" (o XP de batalha é concedido por outro caminho).
- Não achei a função que soma o XP. Tentativas: `PokePartyAddParam` (`197bb0`, nativo de script) delega a uma função por membro
  (`19aab0`) e ao objeto Pokémon (`[obj+0x98]` = CoreParam; `727fe0` é amizade, `725100`/`725800` EVs/IVs por índice);
  as constantes de XP do nível 100 não aparecem no código (as curvas vêm de `bin/pokelib/grow_table`); a comparação de perfis
  de funções (fugir x capturar) e a busca de PB7 no heap deram ambiguidade/falsos positivos; as fotos do heap
  (`LARECOMP_SNAP`) funcionam mas o heap tem contadores demais para isolar o delta.
- Caminhos que ainda valeriam: vigiar (`LARECOMP_WATCH`) o XP de um membro **conhecido** com o endereço achado por uma
  captura jogada à mão no mesmo boot; ou achar a função que escreve a tela "Exp. Points" (o valor 20/11 passa por um
  `SetNumber` de mensagem); ou somar o XP por conta própria depois de achar o campo.

## Próximas etapas
_Atualizado em 17/10/2026: os itens 4 e "ligar/desligar no launcher" do 5 foram feitos (Etapa 4 acima, opção
"Batalhas selvagens (experimental)" desde a 0.2.0, nos dois jogos desde a 0.2.1). Falta o resto do 5 (XP dividido, fuga,
o Pokémon sumir do mapa) e o 3._

3. Confirmar o caminho alternativo (`289c4c` → `28bda0`, modo 3) e a captura pós-batalha (`319e20` → `31a0e0`) em
   execução: o ideal é um save perto de uma batalha selvagem nativa (lendário ou Snorlax); sem isso, forçar o
   método e observar.
4. **Protótipo**: no encontro selvagem (`305ad0`), em vez de `28ef70`, montar o pedido de batalha selvagem para o
   Pokémon tocado e iniciar a batalha; no fim, a captura.
5. Detalhes: fugir, experiência, o Pokémon sumir do mapa, ligar/desligar o modo no launcher.
